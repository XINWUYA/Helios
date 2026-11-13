#include "Platform/Platform.h"

#if HELIOS_PLATFORM_APPLE

#include "Pch.h"
#include "MetalRenderAPI.h"
#include "MetalBindings.h"
#include "MetalBuffer.h"
#include "MetalCommon.h"
#include "MetalTexture.h"
#include "MetalVertexArray.h"
#include "Helios/Profiler/Profiler.h"
#include "Helios/Renderer/GPUScopeInterface.h"
#include "Helios/VirtualDevice/DeviceVertexArray.h"
#include "Helios/Renderer/Renderer.h"

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
         * 因此只需覆盖剔除、正面朝向、深度状态与线框填充——混合与颜色写入属于管线对象。 */
        [[nodiscard]] uint64_t HashEncoderRasterState(const RenderRasterState& state)
        {
            uint64_t h = 0;
            MetalHashCombine(h, static_cast<uint64_t>(state.CullMode));
            MetalHashCombine(h, static_cast<uint64_t>(state.FrontFaceType));
            MetalHashCombine(h, state.EnableDepthTest ? 1u : 0u);
            MetalHashCombine(h, state.EnableDepthWrite ? 1u : 0u);
            MetalHashCombine(h, static_cast<uint64_t>(state.DepthCompareFunc));
            MetalHashCombine(h, state.Wireframe ? 1u : 0u);
            return h;
        }

        /* 编码器标签：把已压入的调试组名连成路径。抓帧里的编码器列表是平铺的，
         * 只写最内层会丢掉上级 Pass；一个组都没有时用 fallback 兜底。 */
        [[nodiscard]] std::string BuildEncoderLabel(const std::vector<std::string>& groups,
            const char* fallback)
        {
            std::string label;
            for (const std::string& group : groups)
            {
                if (group.empty())
                    continue;
                if (!label.empty())
                    label += " / ";
                label += group;
            }

            return label.empty() ? std::string(fallback) : label;
        }
    }

    MetalRenderAPI::~MetalRenderAPI()
    {
        /* 逆序收束：先结束未闭合的 Pass，再注销运行时上下文（此后不再有对象
         * 通过 MetalRuntime 取用设备），最后释放自身持有的设备资源。 */
        CloseRenderEncoder();

        ReleaseDefaultMultisampleAttachments();

        if (m_DefaultDepthTexture)
        {
            m_DefaultDepthTexture->release();
            m_DefaultDepthTexture = nullptr;
        }

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

        for (MTL::CommandBuffer*& command_buffer : m_InFlightCommandBuffers)
        {
            if (command_buffer)
                command_buffer->release();
            command_buffer = nullptr;
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
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

        /* 创建Metal设备 */
        m_Device = MTL::CreateSystemDefaultDevice();
        if (!m_Device)
        {
            CORE_LOG_ERROR("Failed to create Metal device!");
            return;
        }

        CORE_LOG_INFO("Metal Device: {}", m_Device->name()->utf8String());

        /* 间接参数要 Apple3 或 Mac2 起步，iOS 模拟器只报到 Apple2 */
        m_SupportsIndirect = m_Device->supportsFamily(MTL::GPUFamilyApple3)
            || m_Device->supportsFamily(MTL::GPUFamilyMac2);
        if (!m_SupportsIndirect)
            CORE_LOG_WARN("Metal device has no indirect draw or dispatch, GPU driven paths fall back to CPU");

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
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

        ApplyViewport(x_start, y_start, width, height);
    }

    /* 离屏渲染时把视口 y 轴翻转（originY 取到下边界、高度取负），使纹理行序与
     * OpenGL 一致，所有着色器可共用同一套 uv 公式，无需任何后端分支。
     * 渲染到 drawable（屏幕）时不翻转：屏幕行序由系统窗口决定，且只被写入不被采样。 */
    void MetalRenderAPI::ApplyViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
    {
        if (!m_CurrentRenderEncoder || m_EncoderSuspended)
            return;

        /* 当前编码器上已经是这个视口就不再发（Pass 开始时刚按附件尺寸设过一次） */
        if (!TakeViewport(x, y, width, height))
            return;

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
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

        if (m_CurrentRenderEncoder && !m_EncoderSuspended)
        {
            /* 视口高度取自基类记下的 applied 视口，与 y 同属 framebuffer 像素坐标系；
             * 还没设过视口或数值异常时退化为不翻转，避免下溢成巨大值。 */
            const uint32_t viewport_height = GetAppliedViewport().Height;
            const uint32_t flipped_y = (viewport_height > y && viewport_height - y >= height)
                ? (viewport_height - y - height)
                : y;

            /* 裁剪缓存按翻转后的 Metal 坐标记，重复设置直接跳过 */
            if (!TakeScissor(x, flipped_y, width, height))
                return;

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

    uint32_t MetalRenderAPI::GetStateSerial() const
    {
        return MetalRuntime::EncoderGeneration();
    }

    void MetalRenderAPI::ApplyRasterState(RenderRasterState raster_state)
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

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
        const uint32_t key = (static_cast<uint32_t>(compare_func) << 2)
            | (depth_write_enabled ? 1u : 0u);

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
        if (!m_CurrentRenderEncoder || m_EncoderSuspended)
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

        /* 深度测试 / 写入分开下发：透明物体保留测试、关掉写入 —— 否则后画的近处片元
         * 会被先前写入的深度挡住，透明叠层就丢了。只在 RenderPass 有深度附件时才设置
         * （ImGui 叠加层没有，硬设会被 Metal 校验判错）。 */
        if (ActiveRenderPassHasDepthAttachment())
        {
            const CompareFunc compare_func = m_CurrentRasterState.EnableDepthTest
                ? m_CurrentRasterState.DepthCompareFunc
                : CompareFunc::Always;
            const bool depth_write = m_CurrentRasterState.EnableDepthTest
                && m_CurrentRasterState.EnableDepthWrite;

            if (MTL::DepthStencilState* depth_stencil_state =
                GetOrCreateDepthStencilState(compare_func, depth_write))
            {
                m_CurrentRenderEncoder->setDepthStencilState(depth_stencil_state);
            }
        }

        /* 线框填充：三角形只画边（调试线框预览用；普通状态恒为 FILL） */
        m_CurrentRenderEncoder->setTriangleFillMode(m_CurrentRasterState.Wireframe
            ? MTL::TriangleFillModeLines : MTL::TriangleFillModeFill);

        m_RasterStateEncoder = m_CurrentRenderEncoder;
        m_AppliedRasterStateHash = state_hash;
    }

    void MetalRenderAPI::SetBoundShader(MetalShader* shader)
    {
        m_BoundShader = shader;

        /* 纹理绑定（MetalTexture）拿不到 RenderAPI，按运行时上下文里这份查阶段掩码 */
        MetalRuntime::SetBoundShader(shader);
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
        if (!m_CurrentRenderEncoder || m_EncoderSuspended || !m_BoundShader)
            return false;

        const MetalPipelineDesc desc = BuildPipelineDesc(vertex_layout_hash);
        const bool failed_before = m_BoundShader->HasPipelineFailed(desc.Hash());
        MTL::RenderPipelineState* pipeline = m_BoundShader->GetPipelineState(desc);
        if (!pipeline)
        {
            /* 同一组参数只报一次，坏着色器不至于每次绘制刷一条 */
            if (failed_before)
                return false;

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

    void MetalRenderAPI::BindVertexBuffers(const MetalVertexArray& vertex_array)
    {
        const auto& buffers = vertex_array.GetVertexBuffers();
        const size_t count = std::min(buffers.size(), static_cast<size_t>(MetalBinding::MaxVertexBuffers));

        for (size_t i = 0; i < count; ++i)
        {
            auto* metal_buffer = static_cast<MetalVertexBuffer*>(buffers[i].get());
            if (!metal_buffer || !metal_buffer->GetMetalBuffer())
                continue;

            /* 槽位从 VertexBufferBase 起算，与 VertexDescriptor 及 uniform 槽位错开 */
            BindVertexSlot(MetalBinding::VertexBufferBase + static_cast<uint32_t>(i), metal_buffer->GetMetalBuffer(), 0);
        }
    }

    void MetalRenderAPI::BindVertexSlot(uint32_t slot, MTL::Buffer* buffer, uint32_t offset)
    {
        const uint32_t generation = MetalRuntime::EncoderGeneration();
        CachedVertexBuffer& cached = m_AppliedVertexBuffers[slot];
        if (cached.Generation == generation && cached.Buffer == buffer)
        {
            if (cached.Offset != offset)
            {
                m_CurrentRenderEncoder->setVertexBufferOffset(offset, slot);
                cached.Offset = offset;
            }
            return;
        }

        m_CurrentRenderEncoder->setVertexBuffer(buffer, offset, slot);
        cached.Generation = generation;
        cached.Buffer = buffer;
        cached.Offset = offset;
    }

    void MetalRenderAPI::DrawIndexed(PrimitiveType type, const SharedPtr<DeviceVertexArray>& vertex_array,
        uint32_t index_count, uint32_t index_offset)
    {
        DrawIndexedInstanced(type, vertex_array, nullptr, 0, 1, VertexBufferLayout{}, index_count, index_offset);
    }

    void MetalRenderAPI::DrawIndexedInstanced(PrimitiveType type, const SharedPtr<DeviceVertexArray>& vertex_array,
        const SharedPtr<DeviceVertexBuffer>& instance_buffer, uint32_t instance_offset_bytes,
        uint32_t instance_count, const VertexBufferLayout& /*instance_layout*/,
        uint32_t index_count, uint32_t index_offset)
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);


        if (!m_CurrentRenderEncoder || m_EncoderSuspended)
        {
            CORE_LOG_ERROR("No active render encoder!");
            return;
        }

        /* Metal 后端里的设备对象一定是 Metal 实现，绘制路径上不做 RTTI 转换 */
        auto* metal_vertex_array = static_cast<MetalVertexArray*>(vertex_array.get());
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

        auto* metal_index_buffer = static_cast<MetalIndexBuffer*>(index_buffer.get());
        if (!metal_index_buffer)
        {
            CORE_LOG_ERROR("Invalid index buffer type!");
            return;
        }

        /* 绑定顶点缓冲区（占用 MetalBinding::VertexBufferBase 起的槽位） */
        BindVertexBuffers(*metal_vertex_array);
        if (!ApplyPipelineState(m_BoundShader->GetActiveVertexLayoutHash()))
            return;
        if (!BindInstanceBuffer(*metal_vertex_array, instance_buffer, instance_offset_bytes))
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
            index_offset * index_size,  /* 索引偏移（字节） */
            NS::UInteger(std::max(instance_count, 1u))
        );
    }

    void MetalRenderAPI::DrawIndexedIndirect(PrimitiveType type, const SharedPtr<DeviceVertexArray>& vertex_array,
        const SharedPtr<DeviceVertexBuffer>& instance_buffer, uint32_t instance_offset_bytes,
        const SharedPtr<DeviceStorageBuffer>& args_buffer, uint32_t args_offset_bytes,
        const VertexBufferLayout& /*instance_layout*/)
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

        if (!m_CurrentRenderEncoder || m_EncoderSuspended)
        {
            CORE_LOG_ERROR("No active render encoder!");
            return;
        }

        auto* metal_vertex_array = static_cast<MetalVertexArray*>(vertex_array.get());
        if (!metal_vertex_array || !m_BoundShader)
        {
            CORE_LOG_ERROR("DrawIndexedIndirect: vertex array or shader missing!");
            return;
        }

        auto index_buffer = metal_vertex_array->GetIndexBuffer();
        auto* metal_index_buffer = index_buffer != nullptr
            ? static_cast<MetalIndexBuffer*>(index_buffer.get()) : nullptr;
        auto storage = DynamicPtrCast<MetalStorageBuffer>(args_buffer);
        if (metal_index_buffer == nullptr || storage == nullptr || storage->GetMetalBuffer() == nullptr)
        {
            CORE_LOG_ERROR("DrawIndexedIndirect: index buffer or args buffer missing!");
            return;
        }

        BindVertexBuffers(*metal_vertex_array);
        if (!ApplyPipelineState(m_BoundShader->GetActiveVertexLayoutHash()))
            return;
        if (!BindInstanceBuffer(*metal_vertex_array, instance_buffer, instance_offset_bytes))
            return;

        /* 索引数与实例数都在参数块里：count / instanceCount / indexStart / baseVertex / baseInstance */
        m_CurrentRenderEncoder->drawIndexedPrimitives(
            ToMetalPrimitiveType(type),
            ToMetalIndexType(metal_index_buffer->GetIndexType()),
            metal_index_buffer->GetMetalBuffer(),
            0,
            storage->GetMetalBuffer(),
            args_offset_bytes);
    }

    void MetalRenderAPI::DrawArrays(PrimitiveType type, const SharedPtr<DeviceVertexArray>& vertex_array)
    {
        DrawArraysInstanced(type, vertex_array, nullptr, 0, 1, VertexBufferLayout{});
    }

    void MetalRenderAPI::DrawArraysInstanced(PrimitiveType type, const SharedPtr<DeviceVertexArray>& vertex_array,
        const SharedPtr<DeviceVertexBuffer>& instance_buffer, uint32_t instance_offset_bytes,
        uint32_t instance_count, const VertexBufferLayout& /*instance_layout*/)
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

        if (!m_CurrentRenderEncoder || m_EncoderSuspended)
        {
            CORE_LOG_ERROR("No active render encoder!");
            return;
        }

        /* Metal 后端里的设备对象一定是 Metal 实现，绘制路径上不做 RTTI 转换 */
        auto* metal_vertex_array = static_cast<MetalVertexArray*>(vertex_array.get());
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

        BindVertexBuffers(*metal_vertex_array);
        if (!ApplyPipelineState(m_BoundShader->GetActiveVertexLayoutHash()))
            return;
        if (!BindInstanceBuffer(*metal_vertex_array, instance_buffer, instance_offset_bytes))
            return;

        const uint32_t vertex_count = metal_vertex_array->GetVertexCount();

        m_CurrentRenderEncoder->drawPrimitives(
            ToMetalPrimitiveType(type),
            NS::UInteger(0),
            NS::UInteger(vertex_count),
            NS::UInteger(std::max(instance_count, 1u))
        );
    }

    bool MetalRenderAPI::BindInstanceBuffer(const MetalVertexArray& vertex_array,
        const SharedPtr<DeviceVertexBuffer>& instance_buffer, uint32_t instance_offset_bytes)
    {
        if (instance_buffer == nullptr)
            return true;

        auto* metal_buffer = static_cast<MetalVertexBuffer*>(instance_buffer.get());
        if (metal_buffer == nullptr || metal_buffer->GetMetalBuffer() == nullptr)
            return false;

        /* 实例缓冲接在顶点缓冲后面占一个槽位，槽位满了就不画（描述符那边已报错） */
        const uint32_t slot = MetalBinding::VertexBufferBase
            + static_cast<uint32_t>(vertex_array.GetVertexBuffers().size());
        if (slot >= MetalBinding::VertexBufferBase + MetalBinding::MaxVertexBuffers)
            return false;

        BindVertexSlot(slot, metal_buffer->GetMetalBuffer(), instance_offset_bytes);
        return true;
    }

    void MetalRenderAPI::Flush()
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

        /* 命令缓冲区在本帧所有 Pass 记录完成后由 Present 统一提交。
         * 中途 commit 会切断后续 Pass 的编码，故此处不做提交。 */
    }

    void MetalRenderAPI::GenerateMipmap(const SharedPtr<DeviceTexture>& texture)
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

        auto metal_texture = DynamicPtrCast<MetalTexture>(texture);
        if (!metal_texture || !metal_texture->GetMetalTexture())
            return;

        /* Metal 同一时刻只允许一个编码器：先收束当前渲染通道、再插入 blit 编码器。注意：不能另起
         * 命令缓冲区提交（DeviceTexture::GenerateMipmap 就是那么干的）—— 调用方通常在帧录制中，
         * 独立提交会先执行、结果是空纹理。 */
        CloseRenderEncoder();

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

        SetMetalEncoderLabel(blit, "GenerateMipmap");

        blit->generateMipmaps(metal_texture->GetMetalTexture());
        blit->endEncoding();
    }

    SharedPtr<DeviceTexture> MetalRenderAPI::AcquireDefaultTargetSnapshot(bool& top_down)
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

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

        auto metal_dst = DynamicPtrCast<MetalTexture>(m_DefaultTargetSnapshot);
        if (!metal_dst || !metal_dst->GetMetalTexture())
            return nullptr;

        /* 与 CopyTexture 同一模式：先收束可能还在开启的渲染通道，再在当前帧的
         * 命令缓冲区上插入 blit —— 拷到的是"本 Pass 刚画完"的当刻内容 */
        CloseRenderEncoder();

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

        SetMetalEncoderLabel(blit, "CaptureDefaultTarget");

        blit->copyFromTexture(target, 0, 0, MTL::Origin(0, 0, 0), MTL::Size(width, height, 1),
            metal_dst->GetMetalTexture(), 0, 0, MTL::Origin(0, 0, 0));
        blit->endEncoding();

        top_down = true; /* drawable 行序：顶行在前（与离屏纹理的底行在前相反） */
        return m_DefaultTargetSnapshot;
    }

    bool MetalRenderAPI::SubmitAndWait()
    {
        if (m_CurrentCommandBuffer == nullptr)
            return false;

        /* 计算 / 拷贝可能还挂在当前命令缓冲上：提交它，等执行完，CPU 才能安全读回 */
        CloseRenderEncoder();

        m_CurrentCommandBuffer->commit();
        m_CurrentCommandBuffer->waitUntilCompleted();
        m_LastSubmittedCommandBuffer = m_CurrentCommandBuffer;
        m_CurrentCommandBuffer = nullptr;
        return true;
    }

    void MetalRenderAPI::WaitForGPU()
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

        if (m_LastSubmittedCommandBuffer)
            m_LastSubmittedCommandBuffer->waitUntilCompleted();
    }

    void MetalRenderAPI::CopyTexture(const SharedPtr<DeviceTexture>& src, const SharedPtr<DeviceTexture>& dst,
        uint32_t src_layer)
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

        auto metal_src = DynamicPtrCast<MetalTexture>(src);
        auto metal_dst = DynamicPtrCast<MetalTexture>(dst);
        if (!metal_src || !metal_dst || !metal_src->GetMetalTexture() || !metal_dst->GetMetalTexture())
            return;

        MTL::Texture* src_texture = metal_src->GetMetalTexture();
        MTL::Texture* dst_texture = metal_dst->GetMetalTexture();

        /* 注意：2D 数组的层数是 arrayLength（非数组纹理恒为 1、3D 为 1）——
         * depth() 是 3D 深度，在数组纹理上恒 1，不能拿来判层号 */
        if (src_layer >= src_texture->arrayLength())
        {
            CORE_LOG_ERROR("MetalRenderAPI::CopyTexture: layer {} out of range ({})",
                src_layer, src_texture->arrayLength());
            return;
        }

        /* 与 GenerateMipmap 同一模式：Metal 同一时刻只允许一个命令编码器，
         * 先收束可能还在开启的渲染通道，再在当前帧的命令缓冲区上插入 blit ——
         * 拷贝与前后 Pass 保持录制顺序，读到的正是"当刻"内容。 */
        CloseRenderEncoder();

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

        SetMetalEncoderLabel(blit, "CopyTexture");

        blit->copyFromTexture(src_texture, src_layer, 0, MTL::Origin(0, 0, 0),
            MTL::Size(src_texture->width(), src_texture->height(), 1),
            dst_texture, 0, 0, MTL::Origin(0, 0, 0));
        blit->endEncoding();
    }

    void MetalRenderAPI::Present()
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

        /* 仍有未结束（或挂起等合并）的 Pass 时先收束，避免命令缓冲区漏记整个 Pass */
        CloseRenderEncoder();

        if (!m_CurrentCommandBuffer)
        {
            /* 本帧没有任何绘制：命令缓冲区是空的，但帧级池一定要收。
             * 移动端每帧跑在一次 run loop 回调里，池留到下一帧再排空会跨过
             * run loop 自己的池，直接 badPop。 */
            if (m_FrameAutoreleasePool)
            {
                m_FrameAutoreleasePool->drain();
                m_FrameAutoreleasePool = nullptr;
            }
            return;
        }

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

        MTL::CommandBuffer*& in_flight = m_InFlightCommandBuffers[MetalRuntime::FrameIndex()];
        if (in_flight)
            in_flight->release();
        in_flight = m_CurrentCommandBuffer;
        in_flight->retain();

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

        /* 推进帧序号：按帧切片的 uniform 缓冲区据此切换到下一片，
         * 下一帧开头会先等这一片上一轮的命令缓冲跑完 */
        MetalRuntime::AdvanceFrame();
    }

    void MetalRenderAPI::PrepareNextDrawable()
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

        if (!m_MetalLayer)
        {
            CORE_LOG_ERROR("MetalLayer is not set!");
            return;
        }

        /* 上一帧的 drawable 已在 Present 中呈现，这里清空引用避免重复呈现 */
        m_CurrentDrawable = nullptr;
        m_DefaultTargetStarted = false;

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

                /* 默认渲染目标的颜色格式以 drawable 为准，供管线创建使用，
                 * 避免在引擎内固定假定某一种格式。 */
                if (drawable_texture)
                    m_DefaultColorFormat = drawable_texture->pixelFormat();

                /* 按全局 MSAA 设置决定画在哪：开则落在多重采样附件上、通道结束解析回
                 * drawable，关则照旧直画 */
                SyncDefaultMultisampleAttachments(m_RenderPassDescriptor, drawable_texture);

                color_attachment->setLoadAction(MTL::LoadActionClear);

                /* 此处写入清除色仅是兜底：SetClearColor 通常在本帧稍后调用，
                 * 真正的清除色在 BeginRenderPass 时刷新。 */
                color_attachment->setClearColor(MTL::ClearColor(
                    m_ClearColor.r, m_ClearColor.g, m_ClearColor.b, m_ClearColor.a));
            }
        }
    }

    void MetalRenderAPI::ReleaseDefaultMultisampleAttachments()
    {
        if (m_DefaultMSAAColor)
        {
            m_DefaultMSAAColor->release();
            m_DefaultMSAAColor = nullptr;
        }
        if (m_DefaultMSAADepth)
        {
            m_DefaultMSAADepth->release();
            m_DefaultMSAADepth = nullptr;
        }

        m_DefaultMSAAWidth = 0;
        m_DefaultMSAAHeight = 0;
        m_DefaultMSAASamples = 0;
        m_DefaultMSAAColorFormat = MTL::PixelFormatInvalid;
    }

    void MetalRenderAPI::SyncDefaultMultisampleAttachments(MTL::RenderPassDescriptor* descriptor,
        MTL::Texture* drawable_texture)
    {
        if (!descriptor || !drawable_texture)
            return;

        MTL::RenderPassColorAttachmentDescriptor* color_attachment = descriptor->colorAttachments()->object(0);
        MTL::RenderPassDepthAttachmentDescriptor* depth_attachment = descriptor->depthAttachment();
        MTL::RenderPassStencilAttachmentDescriptor* stencil_attachment = descriptor->stencilAttachment();
        if (!color_attachment)
            return;

        /* 窗口侧在创建与缩放时会往深度附件上换新的单采样纹理：附件上挂的不是我们的
         * 多重采样纹理，就说明换了，重新记一份（解析与还原都要用回它） */
        MTL::Texture* depth_on_attachment = depth_attachment ? depth_attachment->texture() : nullptr;
        if (depth_on_attachment != nullptr && depth_on_attachment != m_DefaultMSAADepth)
        {
            if (m_DefaultDepthTexture)
                m_DefaultDepthTexture->release();
            m_DefaultDepthTexture = depth_on_attachment;
            m_DefaultDepthTexture->retain();
        }

        const uint32_t samples = Renderer::GetMSAASampleCount();
        if (samples <= 1)
        {
            /* 关闭：颜色回到 drawable，深度与模板回到窗口纹理 */
            color_attachment->setTexture(drawable_texture);
            color_attachment->setResolveTexture(nullptr);
            color_attachment->setStoreAction(MTL::StoreActionStore);

            if (depth_attachment && m_DefaultDepthTexture)
            {
                depth_attachment->setTexture(m_DefaultDepthTexture);
                depth_attachment->setResolveTexture(nullptr);
                depth_attachment->setStoreAction(MTL::StoreActionStore);
            }
            if (stencil_attachment && m_DefaultDepthTexture)
                stencil_attachment->setTexture(m_DefaultDepthTexture);

            ReleaseDefaultMultisampleAttachments();
            return;
        }

        const uint32_t width = static_cast<uint32_t>(drawable_texture->width());
        const uint32_t height = static_cast<uint32_t>(drawable_texture->height());
        const MTL::PixelFormat color_format = drawable_texture->pixelFormat();
        const bool need_depth = (m_DefaultDepthTexture != nullptr);
        const bool stale = (m_DefaultMSAAColor == nullptr)
            || m_DefaultMSAAWidth != width
            || m_DefaultMSAAHeight != height
            || m_DefaultMSAASamples != samples
            || m_DefaultMSAAColorFormat != color_format
            || (need_depth && m_DefaultMSAADepth == nullptr);

        if (stale)
        {
            ReleaseDefaultMultisampleAttachments();

            MTL::Device* device = MetalRuntime::Device();
            if (device)
            {
                MTL::TextureDescriptor* color_desc = MTL::TextureDescriptor::alloc()->init();
                color_desc->setTextureType(MTL::TextureType2DMultisample);
                color_desc->setPixelFormat(color_format);
                color_desc->setWidth(width);
                color_desc->setHeight(height);
                color_desc->setSampleCount(samples);
                color_desc->setUsage(MTL::TextureUsageRenderTarget);
                color_desc->setStorageMode(MTL::StorageModePrivate);
                m_DefaultMSAAColor = device->newTexture(color_desc);
                color_desc->release();

                if (need_depth)
                {
                    MTL::TextureDescriptor* depth_desc = MTL::TextureDescriptor::alloc()->init();
                    depth_desc->setTextureType(MTL::TextureType2DMultisample);
                    depth_desc->setPixelFormat(m_DefaultDepthTexture->pixelFormat());
                    depth_desc->setWidth(width);
                    depth_desc->setHeight(height);
                    depth_desc->setSampleCount(samples);
                    depth_desc->setUsage(MTL::TextureUsageRenderTarget);
                    depth_desc->setStorageMode(MTL::StorageModePrivate);
                    m_DefaultMSAADepth = device->newTexture(depth_desc);
                    depth_desc->release();
                }
            }

            m_DefaultMSAAWidth = width;
            m_DefaultMSAAHeight = height;
            m_DefaultMSAASamples = samples;
            m_DefaultMSAAColorFormat = color_format;
        }

        /* 创建失败时退回单采样直画，至少不会渲染到空附件 */
        if (!m_DefaultMSAAColor || (need_depth && !m_DefaultMSAADepth))
        {
            CORE_LOG_WARN("Failed to create multisample attachments for the default target, falling back");

            color_attachment->setTexture(drawable_texture);
            color_attachment->setResolveTexture(nullptr);
            color_attachment->setStoreAction(MTL::StoreActionStore);

            if (depth_attachment && m_DefaultDepthTexture)
                depth_attachment->setTexture(m_DefaultDepthTexture);
            if (stencil_attachment && m_DefaultDepthTexture)
                stencil_attachment->setTexture(m_DefaultDepthTexture);
            return;
        }

        /* 画在多重采样附件上、通道结束解析回 drawable；深度只在通道内使用，不解析。
         * 多重采样纹理要一起存：叠加层（ImGui）用 Load 接着画，只解析不存会读到未定义内容。 */
        color_attachment->setTexture(m_DefaultMSAAColor);
        color_attachment->setResolveTexture(drawable_texture);
        color_attachment->setStoreAction(MTL::StoreActionStoreAndMultisampleResolve);

        if (depth_attachment && m_DefaultMSAADepth)
        {
            depth_attachment->setTexture(m_DefaultMSAADepth);
            depth_attachment->setResolveTexture(nullptr);
            depth_attachment->setStoreAction(MTL::StoreActionStore);
        }
        if (stencil_attachment && m_DefaultMSAADepth)
            stencil_attachment->setTexture(m_DefaultMSAADepth);
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

        /* 光靠 drawable 数量限不住在途帧（启动阶段 nextDrawable 不阻塞），
         * 写这一片 uniform 之前先等上一轮用它的命令缓冲跑完 */
        if (MTL::CommandBuffer*& in_flight = m_InFlightCommandBuffers[MetalRuntime::FrameIndex()])
        {
            in_flight->waitUntilCompleted();
            in_flight->release();
            in_flight = nullptr;
        }

        PrepareNextDrawable();
    }

    void MetalRenderAPI::BeginDefaultRenderPass(bool preserve_content)
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

        if (!m_RenderPassDescriptor)
        {
            CORE_LOG_ERROR("RenderPassDescriptor is not set for default render pass!");
            return;
        }

        /* 渲染图里没声明附件的 Pass 都会开默认通道（如探针烘焙），排在场景之后再清一次就把画面抹掉了 */
        preserve_content = preserve_content || m_DefaultTargetStarted;
        m_DefaultTargetStarted = true;

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

            /* 开 MSAA 时颜色画在多重采样附件上，继续在同一张附件上画就保留了前序内容；
             * 只有单采样才需要把 drawable 重新挂回来 */
            if (color_attachment->texture() != m_DefaultMSAAColor)
                color_attachment->setTexture(m_CurrentDrawable->texture());

            color_attachment->setLoadAction(MTL::LoadActionLoad);
        }

        BeginRenderPass(m_RenderPassDescriptor);
    }

    void MetalRenderAPI::EndDefaultRenderPass()
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);
        EndRenderPass();
    }

    /* Debug 组同时驱动 GPU 耗时作用域：组名是抓帧分组与统计面板共用的唯一标签源
     * （Xcode 的编码器分组 / 面板的作用域树同名同层级）。作用域无条件配对，
     * 不依赖编码器是否已创建 —— 调用点（如阴影层）常先 push 组、再 Bind 开通道。 */
    void MetalRenderAPI::PushDebugGroup(const char* name)
    {
        if (name)
        {
            if (GPUScopeInterface* scope_interface = GetGPUScopeInterface())
                scope_interface->OnGpuDebugGroupBegin(name);
        }

        /* 组名留一份：随后创建的编码器拿它当标签 */
        m_DebugGroupStack.emplace_back(name ? name : "");

        if (m_CurrentRenderEncoder && !m_EncoderSuspended && name)
        {
            m_CurrentRenderEncoder->pushDebugGroup(NS::String::string(name, NS::UTF8StringEncoding));
            ++m_EncoderDebugGroupDepth;
        }
    }

    void MetalRenderAPI::PopDebugGroup()
    {
        if (GPUScopeInterface* scope_interface = GetGPUScopeInterface())
            scope_interface->OnGpuDebugGroupEnd();

        if (!m_DebugGroupStack.empty())
            m_DebugGroupStack.pop_back();

        if (m_CurrentRenderEncoder && !m_EncoderSuspended && m_EncoderDebugGroupDepth > 0)
        {
            m_CurrentRenderEncoder->popDebugGroup();
            --m_EncoderDebugGroupDepth;
        }
    }

    bool MetalRenderAPI::DispatchCompute(const SharedPtr<DeviceShader>& shader,
        const std::vector<std::pair<uint32_t, SharedPtr<DeviceStorageBuffer>>>& buffers,
        uint32_t param_binding, const void* param_data, uint32_t param_size,
        uint32_t group_count)
    {
        return DispatchComputeImpl(shader, buffers, param_binding, param_data, param_size,
            group_count, nullptr, 0);
    }

    bool MetalRenderAPI::DispatchComputeIndirect(const SharedPtr<DeviceShader>& shader,
        const std::vector<std::pair<uint32_t, SharedPtr<DeviceStorageBuffer>>>& buffers,
        uint32_t param_binding, const void* param_data, uint32_t param_size,
        const SharedPtr<DeviceStorageBuffer>& args, uint32_t args_offset)
    {
        if (!m_SupportsIndirect)
            return false;
        auto* storage = args != nullptr
            ? dynamic_cast<MetalStorageBuffer*>(args.get()) : nullptr;
        if (storage == nullptr || storage->GetMetalBuffer() == nullptr)
            return false;
        return DispatchComputeImpl(shader, buffers, param_binding, param_data, param_size,
            0, storage->GetMetalBuffer(), args_offset);
    }

    bool MetalRenderAPI::DispatchComputeImpl(const SharedPtr<DeviceShader>& shader,
        const std::vector<std::pair<uint32_t, SharedPtr<DeviceStorageBuffer>>>& buffers,
        uint32_t param_binding, const void* param_data, uint32_t param_size,
        uint32_t group_count, MTL::Buffer* indirect_args, NS::UInteger indirect_offset)
    {
        auto* compute_shader = dynamic_cast<MetalShader*>(shader.get());
        if (compute_shader == nullptr || !compute_shader->IsCompute())
            return false;

        /* 帧还没开始（Scene 更新阶段）就先借一帧的命令缓冲：整帧共用，最后由 Present 提交 */
        if (m_CurrentCommandBuffer == nullptr && m_CommandQueue != nullptr)
            m_CurrentCommandBuffer = m_CommandQueue->commandBuffer();
        if (m_CurrentCommandBuffer == nullptr)
            return false;

        MTL::ComputePipelineState* pipeline = compute_shader->GetComputePipelineState();
        if (pipeline == nullptr)
            return false;

        /* 计算编码器不能和渲染编码器并存：先把当前的收掉（与 AcquireDefaultTargetSnapshot 同套路） */
        CloseRenderEncoder();

        /* 着色器 binding → MSL buffer 下标：uniform 块在引擎预留的高位区间
         * （见 MetalBinding::ToBufferIndex），storage 缓冲直接用 binding */
        const auto& uniform_blocks = shader->GetReflectionData().UniformBlockBindings;
        const auto to_buffer_index = [&uniform_blocks](uint32_t binding)
        {
            return uniform_blocks.find(binding) != uniform_blocks.end()
                ? MetalBinding::ToBufferIndex(binding) : binding;
        };

        /* 先全部解析成功再开编码器：半路失败时不会留下没结束的编码器 */
        std::vector<std::pair<uint32_t, MTL::Buffer*>> resolved;
        resolved.reserve(buffers.size());
        for (const auto& [binding, buffer] : buffers)
        {
            auto storage = DynamicPtrCast<MetalStorageBuffer>(buffer);
            if (storage == nullptr || storage->GetMetalBuffer() == nullptr)
                return false;
            resolved.emplace_back(to_buffer_index(binding), storage->GetMetalBuffer());
        }

        /* GPU 计时（计算通道）：作用域先于采样点开 —— 通道号只派发给已打开的作用域，
         * 反过来的话这次派发就没有自己的节点。中途建不出编码器也由它自动配对。 */
        const std::string timer_label = std::string("Compute ") + compute_shader->GetDebugName();
        PROFILE_GPU_SCOPE(timer_label);

        /* GPU 计时（通道采样）：计算通道同样要挂时间戳采样点 —— Apple GPU 只在通道
         * 边界采样，不挂的话纯计算的 Pass（如 GPU 蒙皮）在统计面板里永远没有数据。
         * 描述符每次派发新建，所以只有计时开着才建。 */
        MTL::ComputePassDescriptor* compute_descriptor = nullptr;
        if (IsGpuTimingActive())
        {
            compute_descriptor = MTL::ComputePassDescriptor::computePassDescriptor();
            if (GPUScopeInterface* scope_interface = GetGPUScopeInterface())
                scope_interface->OnGpuComputePassBegin(compute_descriptor);
        }

        MTL::ComputeCommandEncoder* encoder = compute_descriptor != nullptr
            ? m_CurrentCommandBuffer->computeCommandEncoder(compute_descriptor)
            : m_CurrentCommandBuffer->computeCommandEncoder();
        if (encoder == nullptr && compute_descriptor != nullptr)
            encoder = m_CurrentCommandBuffer->computeCommandEncoder(); /* 带采样的通道建不出来：退回普通编码器 */
        if (encoder == nullptr)
            return false;

        SetMetalEncoderLabel(encoder, timer_label);

        encoder->setComputePipelineState(pipeline);
        for (const auto& [index, buffer] : resolved)
            encoder->setBuffer(buffer, 0, index);

        /* 常量块走 setBytes：每次派发独立副本，不占常驻缓冲 */
        if (param_data != nullptr && param_size > 0)
            encoder->setBytes(param_data, param_size, to_buffer_index(param_binding));

        const MTL::Size threads_per_group(compute_shader->GetThreadGroupSize(), 1, 1);
        if (indirect_args != nullptr)
            encoder->dispatchThreadgroups(indirect_args, indirect_offset, threads_per_group);
        else
            encoder->dispatchThreadgroups(MTL::Size(group_count, 1, 1), threads_per_group);
        encoder->endEncoding();
        return true;
    }

    void MetalRenderAPI::BeginRenderPass(MTL::RenderPassDescriptor* renderPassDescriptor)
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

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

        /* 上一趟挂起着、附件又完全相同：接着在同一个编码器上画，附件不用写回再读入 */
        if (m_EncoderSuspended && CanResumeRenderPass(renderPassDescriptor))
        {
            m_EncoderSuspended = false;
            m_ActiveRenderPassDescriptor = renderPassDescriptor;
            MetalRuntime::SetEncoder(m_CurrentRenderEncoder);
            ++m_MergedRenderPassCount;

            /* 抓帧里按 Pass 分组：续画的这一趟单独压一层调试组 */
            if (!m_DebugGroupStack.empty() && !m_DebugGroupStack.back().empty())
            {
                m_CurrentRenderEncoder->pushDebugGroup(
                    NS::String::string(m_DebugGroupStack.back().c_str(), NS::UTF8StringEncoding));
                m_ResumeDebugGroupOpen = true;
            }

            /* 同一个编码器上的状态都还在；视口和裁剪按新开一趟的样子复位 */
            uint32_t width = 0;
            uint32_t height = 0;
            GetRenderPassAttachmentSize(renderPassDescriptor, width, height);
            ResetAppliedViewport();
            if (width > 0 && height > 0)
            {
                ApplyViewport(0, 0, width, height);
                m_CurrentRenderEncoder->setScissorRect(MTL::ScissorRect{ 0, 0, width, height });
            }
            return;
        }

        /* Metal 同一时刻只允许一个活动的命令编码器：上一个没收的先收掉 */
        CloseRenderEncoder();

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
        if (GPUScopeInterface* scope_interface = GetGPUScopeInterface())
            scope_interface->OnGpuRenderPassBegin(renderPassDescriptor);

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
        SnapshotAttachments(renderPassDescriptor);
        m_EncoderDebugGroupDepth = 0;

        /* 编码器标签取当前打开的调试组链（最内层就是本趟 Pass 的名字） */
        SetMetalEncoderLabel(m_CurrentRenderEncoder, BuildEncoderLabel(m_DebugGroupStack, "RenderPass"));

        /* 区分离屏 Pass 与默认（drawable）Pass：窗口的 RenderPassDescriptor 是由
         * SetRenderPassDescriptor 登记的那一个，其余描述符都来自 FrameBuffer。 */
        m_CurrentPassIsOffscreen = (renderPassDescriptor != m_RenderPassDescriptor);

        /* 离屏 Pass 先按整个附件尺寸建立一次翻转视口（见 ApplyViewport 说明），
         * 使未显式调用 SetViewport 的 Pass 也不会落到 Metal 默认的"不翻转"视口上。 */
        uint32_t attachment_width = 0;
        uint32_t attachment_height = 0;
        GetRenderPassAttachmentSize(renderPassDescriptor, attachment_width, attachment_height);

        /* 新编码器上视口和裁剪都是空的 */
        ResetAppliedViewport();

        if (m_CurrentPassIsOffscreen && attachment_width > 0 && attachment_height > 0)
            ApplyViewport(0, 0, attachment_width, attachment_height);

        /* 新编码器上没有任何状态，缓存全部失效后统一重新下发一次 */
        m_RasterStateEncoder = nullptr;
        m_ActivePipelineState = nullptr;
        ApplyEncoderRasterState();
    }

    void MetalRenderAPI::EndRenderPass(bool allow_merge)
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

        if (m_CurrentRenderEncoder == nullptr || m_EncoderSuspended)
            return;

        /* 续画时补的那层调试组在这一趟结束时弹掉 */
        if (m_ResumeDebugGroupOpen)
        {
            m_CurrentRenderEncoder->popDebugGroup();
            m_ResumeDebugGroupOpen = false;
        }

        /* 可以合并就先挂着，等下一趟来接；接不上的话由下一次换编码器时统一收掉。
         * 计时开着时每趟都要独立的通道采样，不挂起 */
        if (allow_merge && m_EncoderDebugGroupDepth == 0 && !IsGpuTimingActive())
        {
            m_EncoderSuspended = true;
            MetalRuntime::SetEncoder(nullptr);
            return;
        }

        CloseRenderEncoder();
    }

    void MetalRenderAPI::CloseRenderEncoder()
    {
        if (m_CurrentRenderEncoder)
        {
            if (m_ResumeDebugGroupOpen)
                m_CurrentRenderEncoder->popDebugGroup();
            m_CurrentRenderEncoder->endEncoding();
            if (GPUScopeInterface* scope_interface = GetGPUScopeInterface())
                scope_interface->OnGpuRenderPassEnd();
            m_CurrentRenderEncoder = nullptr;
            MetalRuntime::SetEncoder(nullptr);
        }

        m_EncoderSuspended = false;
        m_ResumeDebugGroupOpen = false;
        m_EncoderDebugGroupDepth = 0;
        m_ActiveRenderPassDescriptor = nullptr;
        m_ActivePipelineState = nullptr;
        m_RasterStateEncoder = nullptr;
    }

    void MetalRenderAPI::SnapshotAttachments(MTL::RenderPassDescriptor* descriptor)
    {
        const auto snapshot = [](MTL::RenderPassAttachmentDescriptor* attachment, AttachmentIdentity& identity)
        {
            identity = AttachmentIdentity{};
            if (attachment == nullptr)
                return;
            identity.Texture = attachment->texture();
            identity.Resolve = attachment->resolveTexture();
            identity.Level = attachment->level();
            identity.Slice = attachment->slice();
        };

        for (uint32_t i = 0; i < MAX_COLOR_ATTACHMENT_NUM; ++i)
            snapshot(descriptor->colorAttachments()->object(i), m_ActiveAttachments[i]);
        snapshot(descriptor->depthAttachment(), m_ActiveAttachments[MAX_COLOR_ATTACHMENT_NUM]);
        snapshot(descriptor->stencilAttachment(), m_ActiveAttachments[MAX_COLOR_ATTACHMENT_NUM + 1]);
    }

    /* 能接着画的条件：附件逐个相同（纹理、解析目标、层级），新一趟全是 Load，也就是它只想在
     * 上一趟的结果上继续叠加。写回方式沿用第一趟的（多写回一次不影响结果） */
    bool MetalRenderAPI::CanResumeRenderPass(MTL::RenderPassDescriptor* descriptor) const
    {
        if (descriptor == nullptr || descriptor == m_RenderPassDescriptor || !m_CurrentPassIsOffscreen)
            return false;
        const auto same = [](MTL::RenderPassAttachmentDescriptor* attachment, const AttachmentIdentity& identity)
        {
            if (attachment == nullptr || attachment->texture() == nullptr)
                return identity.Texture == nullptr;
            return attachment->texture() == identity.Texture
                && attachment->resolveTexture() == identity.Resolve
                && attachment->level() == identity.Level
                && attachment->slice() == identity.Slice
                && attachment->loadAction() == MTL::LoadActionLoad;
        };

        for (uint32_t i = 0; i < MAX_COLOR_ATTACHMENT_NUM; ++i)
        {
            if (!same(descriptor->colorAttachments()->object(i), m_ActiveAttachments[i]))
                return false;
        }
        return same(descriptor->depthAttachment(), m_ActiveAttachments[MAX_COLOR_ATTACHMENT_NUM])
            && same(descriptor->stencilAttachment(), m_ActiveAttachments[MAX_COLOR_ATTACHMENT_NUM + 1]);
    }
}

#endif /* HELIOS_PLATFORM_APPLE */
