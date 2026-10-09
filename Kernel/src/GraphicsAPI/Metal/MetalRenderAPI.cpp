#ifdef PLATFORM_MACOS

#include "Pch.h"
#include "MetalRenderAPI.h"
#include "MetalCommon.h"
#include "MetalTexture.h"
#include "MetalVertexArray.h"
#include "Helios/VirtualDevice/DeviceVertexArray.h"
#include "Helios/Renderer/RenderQuery.h"

namespace Helios
{
    namespace
    {
        /* 读取渲染通道附件纹理的像素格式。附件或纹理缺失时返回 Invalid，
         * 使“该附件不存在”与“附件存在但格式未知”两种情形都能被上层区分。 */
        [[nodiscard]] MTL::PixelFormat GetAttachmentPixelFormat(MTL::RenderPassAttachmentDescriptor* attachment)
        {
            if (!attachment)
                return MTL::PixelFormatInvalid;

            MTL::Texture* texture = attachment->texture();
            return texture ? texture->pixelFormat() : MTL::PixelFormatInvalid;
        }

        /* 取 Pass 附件的像素尺寸（优先颜色附件，其次深度附件），
         * 用于在进入 Pass 时建立覆盖整个附件的默认视口。 */
        void GetRenderPassAttachmentSize(MTL::RenderPassDescriptor* descriptor,
            uint32_t& width, uint32_t& height)
        {
            width = 0;
            height = 0;

            if (!descriptor)
                return;

            MTL::Texture* texture = nullptr;
            if (MTL::RenderPassColorAttachmentDescriptor* color = descriptor->colorAttachments()->object(0))
                texture = color->texture();
            if (!texture)
            {
                if (MTL::RenderPassDepthAttachmentDescriptor* depth = descriptor->depthAttachment())
                    texture = depth->texture();
            }
            if (!texture)
                return;

            width = static_cast<uint32_t>(texture->width());
            height = static_cast<uint32_t>(texture->height());
        }

        /* 影响编码器级状态的字段哈希。仅用于判断状态是否与上一次下发的一致，
         * 因此只需覆盖剔除、正面朝向与深度状态——混合与颜色写入属于管线对象。 */
        [[nodiscard]] uint64_t HashEncoderRasterState(const RenderRasterState& state)
        {
            uint64_t h = 0;
            MetalHashCombine(h, static_cast<uint64_t>(state.CullMode));
            MetalHashCombine(h, static_cast<uint64_t>(state.FrontFaceType));
            MetalHashCombine(h, state.EnableDepthWrite ? 1u : 0u);
            MetalHashCombine(h, static_cast<uint64_t>(state.DepthCompareFunc));
            return h;
        }
    }

    MetalRenderAPI::~MetalRenderAPI()
    {
        /* 逆序收束：先结束未闭合的 Pass，再注销运行时上下文（此后不再有对象
         * 通过 MetalRuntime 取用设备），最后释放自身持有的设备资源。 */
        if (m_CurrentRenderEncoder)
        {
            m_CurrentRenderEncoder->endEncoding();
            m_CurrentRenderEncoder = nullptr;
        }
        m_ActiveRenderPassDescriptor = nullptr;
        m_ActivePipelineState = nullptr;
        m_RasterStateEncoder = nullptr;

        /* 兜底排空可能残留的帧级 autorelease pool */
        if (m_FrameAutoreleasePool)
        {
            m_FrameAutoreleasePool->drain();
            m_FrameAutoreleasePool = nullptr;
        }

        MetalRuntime::Unregister();

        for (auto& [key, state] : m_DepthStencilStates)
        {
            if (state)
                state->release();
        }
        m_DepthStencilStates.clear();

        if (m_LastSubmittedCommandBuffer)
        {
            m_LastSubmittedCommandBuffer->release();
            m_LastSubmittedCommandBuffer = nullptr;
        }

        if (m_CommandQueue)
        {
            m_CommandQueue->release();
            m_CommandQueue = nullptr;
        }
        if (m_Device)
        {
            m_Device->release();
            m_Device = nullptr;
        }
    }

    void MetalRenderAPI::Init()
    {
        PROFILE_FUNCTION();

        /* 创建Metal设备 */
        m_Device = MTL::CreateSystemDefaultDevice();
        if (!m_Device)
        {
            CORE_LOG_ERROR("Failed to create Metal device!");
            return;
        }

        CORE_LOG_INFO("Metal Device: {}", m_Device->name()->utf8String());

#ifdef DEBUG
        /* 调试模式下输出设备信息 */
        CORE_LOG_INFO("Metal Debug Mode: Enabled");
        CORE_LOG_INFO("    Max Buffer Length: {} MB", m_Device->maxBufferLength() / (1024 * 1024));
        CORE_LOG_INFO("    Supports Raytracing: {}", m_Device->supportsRaytracing() ? "Yes" : "No");
        CORE_LOG_INFO("    Supports ShaderBarycentricCoordinates: {}", 
            m_Device->supportsShaderBarycentricCoordinates() ? "Yes" : "No");
#endif

        /* 创建命令队列 */
        m_CommandQueue = m_Device->newCommandQueue();
        if (!m_CommandQueue)
        {
            CORE_LOG_ERROR("Failed to create Metal command queue!");
            return;
        }

        /* 注册运行时上下文：其余 Device 子类（纹理、查询、管线等）据此获取
         * 设备与命令队列，避免各自持有裸指针或依赖 dynamic_cast。 */
        MetalRuntime::Register(m_Device, m_CommandQueue);

        CORE_LOG_INFO("MetalRenderAPI initialized successfully.");
    }

    void MetalRenderAPI::SetViewport(uint32_t x_start, uint32_t y_start, uint32_t width, uint32_t height)
    {
        PROFILE_FUNCTION();

        m_CurrentWidth = width;
        m_CurrentHeight = height;

        if (m_CurrentRenderEncoder)
            ApplyViewport(x_start, y_start, width, height);
    }

    /* 离屏渲染时把视口 y 轴翻转（originY 取到下边界、高度取负），使纹理行序与
     * OpenGL 一致，所有着色器可共用同一套 uv 公式，无需任何后端分支。
     * 渲染到 drawable（屏幕）时不翻转：屏幕行序由系统窗口决定，且只被写入不被采样。 */
    void MetalRenderAPI::ApplyViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
    {
        MTL::Viewport viewport;
        viewport.originX = static_cast<double>(x);
        viewport.width = static_cast<double>(width);
        viewport.znear = 0.0;
        viewport.zfar = 1.0;

        if (m_CurrentPassIsOffscreen)
        {
            viewport.originY = static_cast<double>(y + height);
            viewport.height = -static_cast<double>(height);
        }
        else
        {
            viewport.originY = static_cast<double>(y);
            viewport.height = static_cast<double>(height);
        }

        m_CurrentRenderEncoder->setViewport(viewport);
    }

    /* 裁剪矩形的 y 方向和视口同源：调用方按 OpenGL 约定给"原点左下"的矩形，而 Metal framebuffer
     * 原点在左上，需要翻转（否则裁剪框上下镜像、内容被剔掉）。转换只在本后端内部完成。 */
    void MetalRenderAPI::SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
    {
        PROFILE_FUNCTION();

        if (m_CurrentRenderEncoder)
        {
            /* m_CurrentHeight 为当前视口高度（由 SetViewport 写入），与 y 同属
             * framebuffer 像素坐标系；数值异常时退化为不翻转，避免下溢成巨大值。 */
            const uint32_t flipped_y = (m_CurrentHeight > y && m_CurrentHeight - y >= height)
                ? (m_CurrentHeight - y - height)
                : y;

            MTL::ScissorRect scissorRect;
            scissorRect.x = x;
            scissorRect.y = flipped_y;
            scissorRect.width = width;
            scissorRect.height = height;
            m_CurrentRenderEncoder->setScissorRect(scissorRect);
        }
    }

    void MetalRenderAPI::SetClearColor(const glm::vec4& color)
    {
        m_ClearColor = color;
    }

    void MetalRenderAPI::Clear()
    {
        /* Metal 的清除动作由 Pass 的 loadAction 承担：默认渲染目标在
         * BeginRenderPass 时写入 m_ClearColor，离屏 FrameBuffer 在创建/更新
         * 描述符时写入，因此这里无需（也无法）在编码中途发起清除。 */
    }

    void MetalRenderAPI::ApplyRasterState(RenderRasterState raster_state)
    {
        PROFILE_FUNCTION();

        m_CurrentRasterState = raster_state;

        /* 记录状态即可：管线对象（混合/写掩码）与编码器深度状态都在绘制时
         * 依据“状态 + 当前 Pass 附件格式”一并落地，避免两处先后设置互相覆盖。 */
        ApplyEncoderRasterState();
    }

    bool MetalRenderAPI::ActiveRenderPassHasDepthAttachment() const
    {
        if (!m_ActiveRenderPassDescriptor)
            return false;

        auto depth_attachment = m_ActiveRenderPassDescriptor->depthAttachment();
        return depth_attachment && depth_attachment->texture();
    }

    MTL::DepthStencilState* MetalRenderAPI::GetOrCreateDepthStencilState(CompareFunc compare_func, bool depth_write_enabled)
    {
        const uint32_t key = (static_cast<uint32_t>(compare_func) << 1) | (depth_write_enabled ? 1u : 0u);

        auto it = m_DepthStencilStates.find(key);
        if (it != m_DepthStencilStates.end())
            return it->second;

        if (!m_Device)
            return nullptr;

        MTL::DepthStencilDescriptor* descriptor = MTL::DepthStencilDescriptor::alloc()->init();
        descriptor->setDepthCompareFunction(ToMetalCompareFunction(compare_func));
        descriptor->setDepthWriteEnabled(depth_write_enabled);

        MTL::DepthStencilState* depth_stencil_state = m_Device->newDepthStencilState(descriptor);
        descriptor->release();

        if (!depth_stencil_state)
        {
            CORE_LOG_ERROR("Failed to create depth stencil state (compare={}, write={})",
                static_cast<int>(compare_func), depth_write_enabled);
            return nullptr;
        }

        m_DepthStencilStates[key] = depth_stencil_state;
        return depth_stencil_state;
    }

    void MetalRenderAPI::ApplyEncoderRasterState()
    {
        if (!m_CurrentRenderEncoder)
            return;

        /* 同一编码器内状态未变化时无需重复下发 */
        const uint64_t state_hash = HashEncoderRasterState(m_CurrentRasterState);
        if (m_RasterStateEncoder == m_CurrentRenderEncoder && m_AppliedRasterStateHash == state_hash)
            return;

        m_CurrentRenderEncoder->setCullMode(ToMetalCullMode(m_CurrentRasterState.CullMode));

        /* 绕序补偿：离屏 Pass 的视口 y 翻转会反转绕序（Metal 的正反面判定基于
         * 窗口坐标下的有向面积），这里补偿回来，使 "正面 = FrontFaceType" 在屏幕与
         * 离屏两条路径上含义一致，与 OpenGL 后端语义对齐。 */
        const FrontFaceType front_face = m_CurrentPassIsOffscreen
            ? (m_CurrentRasterState.FrontFaceType == FrontFaceType::CW ? FrontFaceType::CCW : FrontFaceType::CW)
            : m_CurrentRasterState.FrontFaceType;
        m_CurrentRenderEncoder->setFrontFacingWinding(ToMetalWinding(front_face));

        /* 深度测试 / 写入：跟 OpenGL 后端一致 —— EnableDepthWrite 同时管测试和写入，关了 = 不测试。
         * 引擎是 Reversed-Z，默认比较 GreaterEqual。只在 RenderPass 有深度附件时才设置（ImGui 叠加层
         * 没有，硬设会被 Metal 校验判错）。 */
        if (ActiveRenderPassHasDepthAttachment())
        {
            const CompareFunc compare_func = m_CurrentRasterState.EnableDepthWrite
                ? m_CurrentRasterState.DepthCompareFunc
                : CompareFunc::Always;

            if (MTL::DepthStencilState* depth_stencil_state =
                GetOrCreateDepthStencilState(compare_func, m_CurrentRasterState.EnableDepthWrite))
            {
                m_CurrentRenderEncoder->setDepthStencilState(depth_stencil_state);
            }
        }

        m_RasterStateEncoder = m_CurrentRenderEncoder;
        m_AppliedRasterStateHash = state_hash;
    }

    MetalPipelineDesc MetalRenderAPI::BuildPipelineDesc(uint64_t vertex_layout_hash) const
    {
        MetalPipelineDesc desc;
        desc.RasterState = m_CurrentRasterState;
        desc.VertexLayoutHash = vertex_layout_hash;
        desc.SampleCount = 1;

        /* 颜色/深度/模板格式全部取自当前 Pass 的附件纹理，使 HDR、多渲染目标
         * 与 sRGB 输出都能得到匹配的管线，而不是在引擎内假定某一固定格式。 */
        if (m_ActiveRenderPassDescriptor)
        {
            MTL::RenderPassColorAttachmentDescriptorArray* color_attachments =
                m_ActiveRenderPassDescriptor->colorAttachments();
            for (uint32_t i = 0; i < MAX_COLOR_ATTACHMENT_NUM; ++i)
            {
                MTL::RenderPassColorAttachmentDescriptor* attachment = color_attachments->object(i);
                if (!attachment || !attachment->texture())
                    break;

                desc.ColorFormats.push_back(attachment->texture()->pixelFormat());

                /* 采样数由附件纹理决定（MSAA 时 Pass 内所有附件必须一致） */
                const uint32_t sample_count =
                    static_cast<uint32_t>(attachment->texture()->sampleCount());
                if (sample_count > 1)
                    desc.SampleCount = sample_count;
            }

            desc.DepthFormat = GetAttachmentPixelFormat(m_ActiveRenderPassDescriptor->depthAttachment());
            desc.StencilFormat = GetAttachmentPixelFormat(m_ActiveRenderPassDescriptor->stencilAttachment());
        }
        else
        {
            /* 尚未进入任何 Pass：以默认渲染目标的颜色格式预建管线 */
            desc.ColorFormats.push_back(m_DefaultColorFormat);
        }

        /* 纯深度 / 模板 Pass（比如 ShadowPass）时 ColorFormats 保持为空：Metal 允许没有颜色附件的
         * 管线（缺省格式 Invalid 恰好对应这种情况），深度 / 模板附件已经单独提供了。 */

        return desc;
    }

    bool MetalRenderAPI::ApplyPipelineState(uint64_t vertex_layout_hash)
    {
        if (!m_CurrentRenderEncoder || !m_BoundShader)
            return false;

        const MetalPipelineDesc desc = BuildPipelineDesc(vertex_layout_hash);
        MTL::RenderPipelineState* pipeline = m_BoundShader->GetPipelineState(desc);
        if (!pipeline)
        {
            std::string attachment_formats;
            for (size_t i = 0; i < desc.ColorFormats.size(); ++i)
            {
                if (!attachment_formats.empty())
                    attachment_formats += ", ";
                attachment_formats += std::to_string(i) + ":" +
                    std::to_string(static_cast<uint32_t>(desc.ColorFormats[i]));
            }
            CORE_LOG_ERROR("Failed to obtain pipeline state for shader '{}' (color attachments [{}], vertex layout {})",
                m_BoundShader->GetDebugName(), attachment_formats, vertex_layout_hash);
            return false;
        }

        /* 管线状态本身与编码器状态相互独立：切换管线后仍需保证深度状态、
         * 剔除模式等编码器级状态与当前光栅状态一致。 */
        if (m_ActivePipelineState != pipeline)
        {
            m_CurrentRenderEncoder->setRenderPipelineState(pipeline);
            m_ActivePipelineState = pipeline;
        }

        ApplyEncoderRasterState();

        /* 材质参数（含着色器内嵌 uniform）逐次绘制都可能不同，必须每次提交 */
        m_BoundShader->CommitMaterialUniforms(m_CurrentRenderEncoder);
        return true;
    }

    void MetalRenderAPI::DrawIndexed(PrimitiveType type, const SharedPtr<DeviceVertexArray>& vertex_array, uint32_t index_count, uint32_t index_offset)
    {
        PROFILE_FUNCTION();

        if (!m_CurrentRenderEncoder)
        {
            CORE_LOG_ERROR("No active render encoder!");
            return;
        }

        auto metal_vertex_array = std::dynamic_pointer_cast<MetalVertexArray>(vertex_array);
        if (!metal_vertex_array)
        {
            CORE_LOG_ERROR("Invalid vertex array type!");
            return;
        }

        if (!m_BoundShader)
        {
            CORE_LOG_ERROR("No shader bound before DrawIndexed!");
            return;
        }

        /* 获取索引缓冲区信息 */
        auto index_buffer = metal_vertex_array->GetIndexBuffer();
        if (!index_buffer)
        {
            CORE_LOG_ERROR("Index buffer is null!");
            return;
        }

        auto metal_index_buffer = std::dynamic_pointer_cast<MetalIndexBuffer>(index_buffer);
        if (!metal_index_buffer)
        {
            CORE_LOG_ERROR("Invalid index buffer type!");
            return;
        }

        /* 绑定顶点缓冲区（占用 MetalBinding::VertexBufferBase 起的槽位） */
        metal_vertex_array->Bind(m_CurrentRenderEncoder);

        /* 顶点布局决定顶点描述符，进而决定管线状态；无有效管线时不能提交 DrawCall */
        if (!ApplyPipelineState(metal_vertex_array->GetVertexDescriptorHash()))
            return;

        const uint32_t count = index_count > 0 ? index_count : index_buffer->GetCount();

        /* 使用索引缓冲区实际的索引类型和偏移步长 */
        const MTL::IndexType index_type = ToMetalIndexType(metal_index_buffer->GetIndexType());
        const uint32_t index_size = (metal_index_buffer->GetIndexType() == IndexType::UInt16)
            ? sizeof(uint16_t) : sizeof(uint32_t);

        m_CurrentRenderEncoder->drawIndexedPrimitives(
            ToMetalPrimitiveType(type),
            count,
            index_type,
            metal_index_buffer->GetMetalBuffer(),
            index_offset * index_size  /* 索引偏移（字节） */
        );
    }

    void MetalRenderAPI::DrawArrays(PrimitiveType type, const SharedPtr<DeviceVertexArray>& vertex_array)
    {
        PROFILE_FUNCTION();

        if (!m_CurrentRenderEncoder)
        {
            CORE_LOG_ERROR("No active render encoder!");
            return;
        }

        auto metal_vertex_array = std::dynamic_pointer_cast<MetalVertexArray>(vertex_array);
        if (!metal_vertex_array)
        {
            CORE_LOG_ERROR("Invalid vertex array type!");
            return;
        }

        if (!m_BoundShader)
        {
            CORE_LOG_ERROR("No shader bound before DrawArrays!");
            return;
        }

        metal_vertex_array->Bind(m_CurrentRenderEncoder);
        if (!ApplyPipelineState(metal_vertex_array->GetVertexDescriptorHash()))
            return;

        const uint32_t vertex_count = metal_vertex_array->GetVertexCount();

        m_CurrentRenderEncoder->drawPrimitives(
            ToMetalPrimitiveType(type),
            NS::UInteger(0),
            NS::UInteger(vertex_count)
        );
    }

    void MetalRenderAPI::Flush()
    {
        PROFILE_FUNCTION();

        /* 命令缓冲区在本帧所有 Pass 记录完成后由 Present 统一提交。
         * 中途 commit 会切断后续 Pass 的编码，故此处不做提交。 */
    }

    void MetalRenderAPI::GenerateMipmap(const SharedPtr<DeviceTexture>& texture)
    {
        PROFILE_FUNCTION();

        auto metal_texture = std::dynamic_pointer_cast<MetalTexture>(texture);
        if (!metal_texture || !metal_texture->GetMetalTexture())
            return;

        /* Metal 同一时刻只允许一个编码器：先收束当前渲染通道、再插入 blit 编码器。注意：不能另起
         * 命令缓冲区提交（DeviceTexture::GenerateMipmap 就是那么干的）—— 调用方通常在帧录制中，
         * 独立提交会先执行、结果是空纹理。 */
        if (m_CurrentRenderEncoder)
        {
            m_CurrentRenderEncoder->endEncoding();
            RenderQueryProfiler::Instance().OnRenderPassEnd();
            m_CurrentRenderEncoder = nullptr;
            MetalRuntime::SetEncoder(nullptr);
            m_ActiveRenderPassDescriptor = nullptr;
            m_ActivePipelineState = nullptr;
            m_RasterStateEncoder = nullptr;
        }

        if (!m_CurrentCommandBuffer)
        {
            CORE_LOG_ERROR("MetalRenderAPI::GenerateMipmap: no active command buffer");
            return;
        }

        MTL::BlitCommandEncoder* blit = m_CurrentCommandBuffer->blitCommandEncoder();
        if (!blit)
        {
            CORE_LOG_ERROR("MetalRenderAPI::GenerateMipmap: failed to create blit encoder");
            return;
        }

        blit->generateMipmaps(metal_texture->GetMetalTexture());
        blit->endEncoding();
    }

    SharedPtr<DeviceTexture> MetalRenderAPI::AcquireDefaultTargetSnapshot(bool& top_down)
    {
        PROFILE_FUNCTION();

        if (m_CurrentDrawable == nullptr || m_CurrentDrawable->texture() == nullptr)
            return nullptr;
        MTL::Texture* target = m_CurrentDrawable->texture();

        const uint32_t width = static_cast<uint32_t>(target->width());
        const uint32_t height = static_cast<uint32_t>(target->height());
        if (width == 0 || height == 0)
            return nullptr;

        /* 预览纹理跨帧复用；drawable 尺寸变化（窗口缩放）时重建。
         * 格式与图层一致（RGBA8，见 MetalWindow 的 setPixelFormat）——
         * blit 要求源 / 目标像素格式相同 */
        if (m_DefaultTargetSnapshot == nullptr
            || m_DefaultTargetSnapshot->GetTextureDesc().Width != width
            || m_DefaultTargetSnapshot->GetTextureDesc().Height != height)
        {
            TextureDesc desc;
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.Samples = 1;
            desc.Format = TextureFormat::RGBA8;
            desc.SamplerType = SamplerType::Sampler2D;
            desc.Usage = TextureUsage::Sampleable;
            m_DefaultTargetSnapshot = DeviceTexture::Create("DefaultTargetSnapshot", desc);
        }
        if (m_DefaultTargetSnapshot == nullptr)
            return nullptr;

        auto metal_dst = std::dynamic_pointer_cast<MetalTexture>(m_DefaultTargetSnapshot);
        if (!metal_dst || !metal_dst->GetMetalTexture())
            return nullptr;

        /* 与 CopyTexture 同一模式：先收束可能还在开启的渲染通道，再在当前帧的
         * 命令缓冲区上插入 blit —— 拷到的是"本 Pass 刚画完"的当刻内容 */
        if (m_CurrentRenderEncoder)
        {
            m_CurrentRenderEncoder->endEncoding();
            RenderQueryProfiler::Instance().OnRenderPassEnd();
            m_CurrentRenderEncoder = nullptr;
            MetalRuntime::SetEncoder(nullptr);
            m_ActiveRenderPassDescriptor = nullptr;
            m_ActivePipelineState = nullptr;
            m_RasterStateEncoder = nullptr;
        }

        if (!m_CurrentCommandBuffer)
        {
            CORE_LOG_ERROR("MetalRenderAPI::AcquireDefaultTargetSnapshot: no active command buffer");
            return nullptr;
        }

        MTL::BlitCommandEncoder* blit = m_CurrentCommandBuffer->blitCommandEncoder();
        if (!blit)
        {
            CORE_LOG_ERROR("MetalRenderAPI::AcquireDefaultTargetSnapshot: failed to create blit encoder");
            return nullptr;
        }

        blit->copyFromTexture(target, 0, 0, MTL::Origin(0, 0, 0), MTL::Size(width, height, 1),
            metal_dst->GetMetalTexture(), 0, 0, MTL::Origin(0, 0, 0));
        blit->endEncoding();

        top_down = true; /* drawable 行序：顶行在前（与离屏纹理的底行在前相反） */
        return m_DefaultTargetSnapshot;
    }

    void MetalRenderAPI::WaitForGPU()
    {
        PROFILE_FUNCTION();

        if (m_LastSubmittedCommandBuffer)
            m_LastSubmittedCommandBuffer->waitUntilCompleted();
    }

    void MetalRenderAPI::CopyTexture(const SharedPtr<DeviceTexture>& src, const SharedPtr<DeviceTexture>& dst)
    {
        PROFILE_FUNCTION();

        auto metal_src = std::dynamic_pointer_cast<MetalTexture>(src);
        auto metal_dst = std::dynamic_pointer_cast<MetalTexture>(dst);
        if (!metal_src || !metal_dst || !metal_src->GetMetalTexture() || !metal_dst->GetMetalTexture())
            return;

        MTL::Texture* src_texture = metal_src->GetMetalTexture();
        MTL::Texture* dst_texture = metal_dst->GetMetalTexture();

        /* 与 GenerateMipmap 同一模式：Metal 同一时刻只允许一个命令编码器，
         * 先收束可能还在开启的渲染通道，再在当前帧的命令缓冲区上插入 blit ——
         * 拷贝与前后 Pass 保持录制顺序，读到的正是"当刻"内容。 */
        if (m_CurrentRenderEncoder)
        {
            m_CurrentRenderEncoder->endEncoding();
            RenderQueryProfiler::Instance().OnRenderPassEnd();
            m_CurrentRenderEncoder = nullptr;
            MetalRuntime::SetEncoder(nullptr);
            m_ActiveRenderPassDescriptor = nullptr;
            m_ActivePipelineState = nullptr;
            m_RasterStateEncoder = nullptr;
        }

        if (!m_CurrentCommandBuffer)
        {
            CORE_LOG_ERROR("MetalRenderAPI::CopyTexture: no active command buffer");
            return;
        }

        MTL::BlitCommandEncoder* blit = m_CurrentCommandBuffer->blitCommandEncoder();
        if (!blit)
        {
            CORE_LOG_ERROR("MetalRenderAPI::CopyTexture: failed to create blit encoder");
            return;
        }

        blit->copyFromTexture(src_texture, 0, 0, MTL::Origin(0, 0, 0),
            MTL::Size(src_texture->width(), src_texture->height(), 1),
            dst_texture, 0, 0, MTL::Origin(0, 0, 0));
        blit->endEncoding();
    }

    void MetalRenderAPI::Present()
    {
        PROFILE_FUNCTION();

        /* 仍有未结束的 Pass 时先收束，避免命令缓冲区漏记整个 Pass */
        if (m_CurrentRenderEncoder)
            EndRenderPass();

        if (!m_CurrentCommandBuffer)
            return;

        if (m_CurrentDrawable)
        {
            m_CurrentCommandBuffer->presentDrawable(m_CurrentDrawable);
            m_CurrentDrawable = nullptr;
        }

        m_CurrentCommandBuffer->commit();

        /* 留一份已提交命令缓冲区的句柄：CPU 回读 GPU 写入的结果前需要等它执行完，
         * 而 Present 之后 m_CurrentCommandBuffer 会被丢弃、autorelease pool 也会排空，
         * 因此额外 retain 一份（模型见 MetalQueryNode 的采样回读）。 */
        if (m_LastSubmittedCommandBuffer)
            m_LastSubmittedCommandBuffer->release();
        m_LastSubmittedCommandBuffer = m_CurrentCommandBuffer;
        m_LastSubmittedCommandBuffer->retain();

        m_CurrentCommandBuffer = nullptr;

        m_ActivePipelineState = nullptr;
        m_RasterStateEncoder = nullptr;

        /* 排空帧级 autorelease pool：本帧创建的 Metal 对象统一释放。commit 之后驱动自己持有内部引用，
         * 释放 ObjC 包装对象是安全的。 */
        if (m_FrameAutoreleasePool)
        {
            m_FrameAutoreleasePool->drain();
            m_FrameAutoreleasePool = nullptr;
        }

        /* 推进帧序号：按帧切片的 uniform 缓冲区据此切换到下一片。
         * 帧数上限由 drawable 数量（nextDrawable 在无可用 drawable 时阻塞）
         * 与 MetalConfig::MaxFramesInFlight 共同保证，二者取值一致。 */
        MetalRuntime::AdvanceFrame();
    }

    void MetalRenderAPI::PrepareNextDrawable()
    {
        PROFILE_FUNCTION();

        if (!m_MetalLayer)
        {
            CORE_LOG_ERROR("MetalLayer is not set!");
            return;
        }

        /* 上一帧的 drawable 已在 Present 中呈现，这里清空引用避免重复呈现 */
        m_CurrentDrawable = nullptr;

        /* 获取下一个可绘制对象 */
        m_CurrentDrawable = m_MetalLayer->nextDrawable();
        if (!m_CurrentDrawable)
        {
            CORE_LOG_ERROR("Failed to get next drawable!");
            return;
        }

        /* 配置渲染通道描述符的颜色附件 */
        if (m_RenderPassDescriptor)
        {
            MTL::RenderPassColorAttachmentDescriptor* color_attachment =
                m_RenderPassDescriptor->colorAttachments()->object(0);
            if (color_attachment)
            {
                MTL::Texture* drawable_texture = m_CurrentDrawable->texture();

                color_attachment->setTexture(drawable_texture);
                color_attachment->setLoadAction(MTL::LoadActionClear);
                color_attachment->setStoreAction(MTL::StoreActionStore);

                /* 默认渲染目标的颜色格式以 drawable 为准，供管线创建使用，
                 * 避免在引擎内固定假定某一种格式。 */
                if (drawable_texture)
                    m_DefaultColorFormat = drawable_texture->pixelFormat();

                /* 此处写入清除色仅是兜底：SetClearColor 通常在本帧稍后调用，
                 * 真正的清除色在 BeginRenderPass 时刷新。 */
                color_attachment->setClearColor(MTL::ClearColor(
                    m_ClearColor.r, m_ClearColor.g, m_ClearColor.b, m_ClearColor.a));
            }
        }
    }

    void MetalRenderAPI::ApplyDefaultClearColor()
    {
        if (!m_RenderPassDescriptor)
            return;

        MTL::RenderPassColorAttachmentDescriptor* color_attachment =
            m_RenderPassDescriptor->colorAttachments()->object(0);
        if (!color_attachment || !color_attachment->texture())
            return;

        color_attachment->setClearColor(MTL::ClearColor(
            m_ClearColor.r, m_ClearColor.g, m_ClearColor.b, m_ClearColor.a));
    }

    void MetalRenderAPI::PrepareNextFrame()
    {
        /* 帧入口建立 autorelease pool（上一帧的池已在 Present 掐尾排空；
         * 若因异常路径残留，这里兜底排空避免叠加）。 */
        if (m_FrameAutoreleasePool)
        {
            m_FrameAutoreleasePool->drain();
            m_FrameAutoreleasePool = nullptr;
        }
        m_FrameAutoreleasePool = NS::AutoreleasePool::alloc()->init();

        PrepareNextDrawable();
    }

    void MetalRenderAPI::BeginDefaultRenderPass(bool preserve_content)
    {
        PROFILE_FUNCTION();

        if (!m_RenderPassDescriptor)
        {
            CORE_LOG_ERROR("RenderPassDescriptor is not set for default render pass!");
            return;
        }

        if (preserve_content)
        {
            /* 叠加层场景（如 ImGui）：把当前 drawable 重新挂为颜色附件并改用
             * LoadActionLoad，使前序 Pass 已经写入 drawable 的内容继续保留。
             * PrepareNextDrawable 每帧会把该附件复位为清除，不会污染下一帧。 */
            MTL::RenderPassColorAttachmentDescriptor* color_attachment =
                m_RenderPassDescriptor->colorAttachments()->object(0);
            if (!color_attachment)
                return;

            if (!m_CurrentDrawable)
            {
                CORE_LOG_ERROR("BeginDefaultRenderPass: no valid drawable available!");
                return;
            }

            color_attachment->setTexture(m_CurrentDrawable->texture());
            color_attachment->setLoadAction(MTL::LoadActionLoad);
        }

        BeginRenderPass(m_RenderPassDescriptor);
    }

    void MetalRenderAPI::EndDefaultRenderPass()
    {
        PROFILE_FUNCTION();
        EndRenderPass();
    }

    /* Debug 组同时驱动 GPU 耗时作用域：组名是抓帧分组与统计面板共用的唯一标签源
     * （Xcode 的编码器分组 / 面板的作用域树同名同层级）。作用域无条件配对，
     * 不依赖编码器是否已创建 —— 调用点（如阴影层）常先 push 组、再 Bind 开通道。 */
    void MetalRenderAPI::PushDebugGroup(const char* name)
    {
        if (name)
            RenderQueryProfiler::Instance().BeginGPUScope(name);

        if (m_CurrentRenderEncoder && name)
        {
            m_CurrentRenderEncoder->pushDebugGroup(NS::String::string(name, NS::UTF8StringEncoding));
        }
    }

    void MetalRenderAPI::PopDebugGroup()
    {
        RenderQueryProfiler::Instance().EndGPUScope();

        if (m_CurrentRenderEncoder)
        {
            m_CurrentRenderEncoder->popDebugGroup();
        }
    }

    void MetalRenderAPI::BeginRenderPass(MTL::RenderPassDescriptor* renderPassDescriptor)
    {
        PROFILE_FUNCTION();

        if (!m_Device || !m_CommandQueue)
        {
            CORE_LOG_ERROR("Metal device or command queue not initialized!");
            return;
        }

        if (!renderPassDescriptor)
        {
            CORE_LOG_ERROR("RenderPassDescriptor is null!");
            return;
        }

        /* Metal 同一时刻只允许一个活动的命令编码器。若上一个 Pass 未显式结束
         * （例如 FrameGraph 先开启默认 Pass 再执行携带有离屏附件的 Pass），
         * 先将其收束，使 Pass 交替时命令缓冲区结构始终有效。 */
        if (m_CurrentRenderEncoder)
        {
            m_CurrentRenderEncoder->endEncoding();
            RenderQueryProfiler::Instance().OnRenderPassEnd();
            m_CurrentRenderEncoder = nullptr;
            m_ActiveRenderPassDescriptor = nullptr;
            m_ActivePipelineState = nullptr;
            m_RasterStateEncoder = nullptr;
        }

        /* 如果没有命令缓冲区，创建一个新的（整帧共用一个，最后由 Present 提交） */
        if (!m_CurrentCommandBuffer)
        {
            m_CurrentCommandBuffer = m_CommandQueue->commandBuffer();
            if (!m_CurrentCommandBuffer)
            {
                CORE_LOG_ERROR("Failed to create command buffer!");
                return;
            }
        }

        /* 默认渲染目标的清除色在进入 Pass 时刷新，保证本帧设置的清除色立即生效 */
        if (renderPassDescriptor == m_RenderPassDescriptor)
            ApplyDefaultClearColor();

        /* GPU 计时（通道采样）：把本通道的一对时间戳采样下标挂到通道描述符上。
         * 必须在创建编码器之前完成（附件在创建时被读取）；无论是否在计时都调用
         * —— 描述符跨帧复用，残留的旧附件由查询缓冲负责清理。 */
        RenderQueryProfiler::Instance().OnRenderPassBegin(renderPassDescriptor);

        /* 创建渲染命令编码器 */
        m_ActiveRenderPassDescriptor = renderPassDescriptor;
        m_CurrentRenderEncoder = m_CurrentCommandBuffer->renderCommandEncoder(renderPassDescriptor);
        if (!m_CurrentRenderEncoder)
        {
            CORE_LOG_ERROR("Failed to create render command encoder!");
            m_ActiveRenderPassDescriptor = nullptr;
            return;
        }

        MetalRuntime::SetEncoder(m_CurrentRenderEncoder);

        /* 区分离屏 Pass 与默认（drawable）Pass：窗口的 RenderPassDescriptor 是由
         * SetRenderPassDescriptor 登记的那一个，其余描述符都来自 FrameBuffer。 */
        m_CurrentPassIsOffscreen = (renderPassDescriptor != m_RenderPassDescriptor);

        /* 离屏 Pass 先按整个附件尺寸建立一次翻转视口（见 ApplyViewport 说明），
         * 使未显式调用 SetViewport 的 Pass 也不会落到 Metal 默认的"不翻转"视口上。 */
        if (m_CurrentPassIsOffscreen)
        {
            uint32_t attachment_width = 0;
            uint32_t attachment_height = 0;
            GetRenderPassAttachmentSize(renderPassDescriptor, attachment_width, attachment_height);
            if (attachment_width > 0 && attachment_height > 0)
                ApplyViewport(0, 0, attachment_width, attachment_height);
        }

        /* 新编码器上没有任何状态，缓存全部失效后统一重新下发一次 */
        m_RasterStateEncoder = nullptr;
        m_ActivePipelineState = nullptr;
        ApplyEncoderRasterState();
    }

    void MetalRenderAPI::EndRenderPass()
    {
        PROFILE_FUNCTION();

        if (m_CurrentRenderEncoder)
        {
            m_CurrentRenderEncoder->endEncoding();
            RenderQueryProfiler::Instance().OnRenderPassEnd();
            m_CurrentRenderEncoder = nullptr;
            MetalRuntime::SetEncoder(nullptr);
        }

        m_ActiveRenderPassDescriptor = nullptr;
        m_ActivePipelineState = nullptr;
        m_RasterStateEncoder = nullptr;
    }
}

#endif /* PLATFORM_MACOS */
