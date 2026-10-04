#ifdef PLATFORM_MACOS

#include "Pch.h"
#include "MetalFrameBuffer.h"
#include "MetalCommon.h"
#include "MetalConversions.h"
#include "MetalTexture.h"
#include "MetalRenderAPI.h"
#include "Helios/Renderer/Renderer.h"

namespace Helios
{
    namespace
    {
        /* Metal 的清除值统一使用 double 分量，整数附件由驱动完成转换 */
        MTL::ClearColor ResolveClearColor(const PixelDesc& pixel_desc, void* data)
        {
            if (!data)
                return MTL::ClearColor(0.0, 0.0, 0.0, 0.0);

            double components[4] = { 0.0, 0.0, 0.0, 0.0 };
            switch (pixel_desc.Type)
            {
            case PixelType::UnsignedByte:
            {
                const auto* values = static_cast<const uint8_t*>(data);
                for (int i = 0; i < 4; ++i)
                    components[i] = static_cast<double>(values[i]) / 255.0;
                break;
            }
            case PixelType::UnsignedInt:
            case PixelType::Int:
            {
                const auto* values = static_cast<const uint32_t*>(data);
                for (int i = 0; i < 4; ++i)
                    components[i] = static_cast<double>(values[i]);
                break;
            }
            case PixelType::Float:
            default:
            {
                const auto* values = static_cast<const float*>(data);
                for (int i = 0; i < 4; ++i)
                    components[i] = static_cast<double>(values[i]);
                break;
            }
            }

            return MTL::ClearColor(components[0], components[1], components[2], components[3]);
        }

        /* 附件 level/slice 复位：Whole 模式或未被定位的附件都使用第 0 层 */
        void ResetAttachmentLocation(MTL::RenderPassAttachmentDescriptor* attachment)
        {
            if (!attachment)
                return;

            attachment->setLevel(0);
            attachment->setSlice(0);
        }
    }

    MetalFrameBuffer::MetalFrameBuffer(const std::string& name, const FrameBufferDesc& desc)
        : DeviceFrameBuffer(name, desc)
    {
        PROFILE_FUNCTION();

        m_RenderPassDescriptor = MTL::RenderPassDescriptor::alloc()->init();

        CreateAttachments();
        UpdateRenderPassDescriptor();

        CORE_LOG_INFO("Metal FrameBuffer created: {} ({}x{}, {} samples)",
            name, desc.ViewportRegion.Width, desc.ViewportRegion.Height, desc.Samples);
    }

    MetalFrameBuffer::~MetalFrameBuffer()
    {
        PROFILE_FUNCTION();

        ReleaseAttachments();

        if (m_RenderPassDescriptor)
        {
            m_RenderPassDescriptor->release();
            m_RenderPassDescriptor = nullptr;
        }
    }

    void MetalFrameBuffer::Bind(const FrameBufferBindInfo& bind_info)
    {
        PROFILE_FUNCTION();

        if (!m_RenderPassDescriptor)
            return;

        auto* render_api = dynamic_cast<MetalRenderAPI*>(Renderer::GetRenderAPI().get());
        if (!render_api)
        {
            CORE_LOG_ERROR("MetalFrameBuffer::Bind requires an active Metal RenderAPI");
            return;
        }

        ApplyAttachmentLocation(bind_info);
        render_api->BeginRenderPass(m_RenderPassDescriptor);
    }

    void MetalFrameBuffer::Unbind()
    {
        PROFILE_FUNCTION();

        auto* render_api = dynamic_cast<MetalRenderAPI*>(Renderer::GetRenderAPI().get());
        if (render_api)
        {
            render_api->EndRenderPass();
        }
    }

    void MetalFrameBuffer::Resize(uint32_t width, uint32_t height)
    {
        PROFILE_FUNCTION();

        if (width == 0 || height == 0 || width > 8192 || height > 8192)
        {
            CORE_LOG_WARN("Attempted to resize framebuffer to {}, {}, but not supported!", width, height);
            return;
        }

        if (width == m_FrameBufferDesc.ViewportRegion.Width &&
            height == m_FrameBufferDesc.ViewportRegion.Height)
        {
            return;
        }

        m_FrameBufferDesc.ViewportRegion.Width = width;
        m_FrameBufferDesc.ViewportRegion.Height = height;

        ReleaseAttachments();
        CreateAttachments();
        UpdateRenderPassDescriptor();
    }

    void MetalFrameBuffer::ReadPixel(uint32_t attachment_index, int x, int y,
        const PixelDesc& pixel_desc, void* data)
    {
        PROFILE_FUNCTION();

        if (!data || attachment_index >= m_ColorAttachments.size())
            return;

        MTL::Texture* texture = m_ColorAttachments[attachment_index];
        if (!texture)
            return;

        /* 多重采样附件与私有存储都不支持 CPU 直接回读。
         * 前者需要先解析，后者需要经由 blit 拷贝到可读纹理，此处明确报错而不是静默返回。 */
        if (texture->sampleCount() > 1)
        {
            CORE_LOG_ERROR("MetalFrameBuffer::ReadPixel: attachment {} is multisampled, resolve it first",
                attachment_index);
            return;
        }

        if (texture->storageMode() == MTL::StorageModePrivate)
        {
            CORE_LOG_ERROR("MetalFrameBuffer::ReadPixel: attachment {} uses private storage",
                attachment_index);
            return;
        }

        const uint32_t bytes_per_pixel = GetPixelDescBytesPerPixel(pixel_desc);
        if (bytes_per_pixel == 0)
        {
            CORE_LOG_ERROR("MetalFrameBuffer::ReadPixel: unsupported pixel desc");
            return;
        }

        MTL::Region region;
        region.origin.x = static_cast<NS::UInteger>(x);
        region.origin.y = static_cast<NS::UInteger>(y);
        region.origin.z = 0;
        region.size.width = 1;
        region.size.height = 1;
        region.size.depth = 1;

        texture->getBytes(data, bytes_per_pixel, region, 0);
    }

    void MetalFrameBuffer::ClearAttachment(uint32_t attachment_index, int level,
        const PixelDesc& pixel_desc, void* data)
    {
        PROFILE_FUNCTION();

        auto* render_api = dynamic_cast<MetalRenderAPI*>(Renderer::GetRenderAPI().get());
        if (!render_api)
            return;

        /* Metal 的清除动作只能在渲染通道开始时执行。若当前已有活动通道，
         * 插入一个只做清除的空通道会破坏正在录制的绘制，因此明确拒绝。 */
        if (render_api->GetCurrentRenderEncoder())
        {
            CORE_LOG_WARN("MetalFrameBuffer::ClearAttachment ignored: a render pass is already active");
            return;
        }

        MTL::RenderPassDescriptor* descriptor = MTL::RenderPassDescriptor::alloc()->init();
        const MTL::ClearColor clear_color = ResolveClearColor(pixel_desc, data);

        for (NS::UInteger i = 0; i < m_ColorAttachments.size(); ++i)
        {
            auto* attachment = descriptor->colorAttachments()->object(i);
            if (i == attachment_index)
            {
                attachment->setTexture(m_ColorAttachments[i]);
                attachment->setLevel(level);
                attachment->setLoadAction(MTL::LoadActionClear);
                attachment->setStoreAction(MTL::StoreActionStore);
                attachment->setClearColor(clear_color);
            }
            else
            {
                /* 未涉及的附件必须保持 DontCare，否则会被一并清除 */
                attachment->setLoadAction(MTL::LoadActionDontCare);
                attachment->setStoreAction(MTL::StoreActionDontCare);
            }
        }

        render_api->BeginRenderPass(descriptor);
        render_api->EndRenderPass();
        descriptor->release();
    }

    MTL::Texture* MetalFrameBuffer::GetColorAttachment(uint32_t index) const
    {
        if (index < m_ColorAttachments.size())
            return m_ColorAttachments[index];
        return nullptr;
    }

    void MetalFrameBuffer::ApplyAttachmentLocation(const FrameBufferBindInfo& bind_info)
    {
        const bool layered = bind_info.Mode == FrameBufferBindMode::Layered;

        const auto locate = [&bind_info](MTL::RenderPassAttachmentDescriptor* attachment)
        {
            if (!attachment)
                return;

            attachment->setLevel(bind_info.MipLevel);
            attachment->setSlice(bind_info.LayerIndex);
        };

        for (NS::UInteger i = 0; i < m_ColorAttachments.size(); ++i)
        {
            auto* attachment = m_RenderPassDescriptor->colorAttachments()->object(i);
            const bool targeted = layered &&
                bind_info.TargetAttachment == FrameBufferAttachment::Color &&
                i == bind_info.ColorIndex;

            if (targeted)
                locate(attachment);
            else
                ResetAttachmentLocation(attachment);
        }

        if (layered && bind_info.TargetAttachment == FrameBufferAttachment::Depth)
        {
            locate(m_RenderPassDescriptor->depthAttachment());
            ResetAttachmentLocation(m_RenderPassDescriptor->stencilAttachment());
        }
        else if (layered && bind_info.TargetAttachment == FrameBufferAttachment::Stencil)
        {
            ResetAttachmentLocation(m_RenderPassDescriptor->depthAttachment());
            locate(m_RenderPassDescriptor->stencilAttachment());
        }
        else
        {
            ResetAttachmentLocation(m_RenderPassDescriptor->depthAttachment());
            ResetAttachmentLocation(m_RenderPassDescriptor->stencilAttachment());
        }
    }

    MTL::Texture* MetalFrameBuffer::AcquireRenderTargetTexture(const RenderBufferInfo& render_buffer)
    {
        auto metal_texture = std::dynamic_pointer_cast<MetalTexture>(render_buffer.RenderTarget);
        if (!metal_texture)
            return nullptr;

        MTL::Texture* texture = metal_texture->GetMetalTexture();
        if (texture)
            texture->retain();

        return texture;
    }

    MTL::Texture* MetalFrameBuffer::CreateAttachmentTexture(const TextureDesc& desc, uint32_t samples)
    {
        MTL::Device* device = MetalRuntime::Device();
        if (!device)
        {
            CORE_LOG_ERROR("MetalFrameBuffer: Metal device unavailable");
            return nullptr;
        }

        const MTL::PixelFormat pixel_format = ToMetalPixelFormat(desc.Format);
        if (pixel_format == MTL::PixelFormatInvalid)
        {
            CORE_LOG_ERROR("MetalFrameBuffer: unsupported attachment format {}", static_cast<int>(desc.Format));
            return nullptr;
        }

        MTL::TextureType texture_type = ToMetalSamplerType(desc.SamplerType);
        if (samples > 1)
        {
            texture_type = desc.SamplerType == SamplerType::Sampler2DArray
                ? MTL::TextureType2DMultisampleArray
                : MTL::TextureType2DMultisample;
        }

        MTL::TextureDescriptor* texture_desc = MTL::TextureDescriptor::alloc()->init();
        texture_desc->setTextureType(texture_type);
        texture_desc->setPixelFormat(pixel_format);
        texture_desc->setWidth(std::max(1u, desc.Width));
        texture_desc->setHeight(std::max(1u, desc.Height));
        texture_desc->setUsage(MTL::TextureUsageRenderTarget);

        if (desc.SamplerType == SamplerType::Sampler2DArray)
            texture_desc->setArrayLength(std::max(1u, desc.Depth));

        if (samples > 1)
        {
            texture_desc->setSampleCount(samples);
        }
        else
        {
            /* 单采样附件保持 CPU 可读，便于 ReadPixel 与截图；多重采样附件无法回读 */
            texture_desc->setStorageMode(static_cast<MTL::StorageMode>(MetalStorage::TextureOptions));
        }

        MTL::Texture* texture = device->newTexture(texture_desc);
        texture_desc->release();
        return texture;
    }

    void MetalFrameBuffer::CreateAttachments()
    {
        const uint32_t width = m_FrameBufferDesc.ViewportRegion.Width;
        const uint32_t height = m_FrameBufferDesc.ViewportRegion.Height;
        const uint32_t samples = std::max(1u, m_FrameBufferDesc.Samples);

        /* 颜色附件：优先复用 RenderTarget；当要求 MSAA 而目标是单采样纹理时，
         * 额外创建多重采样附件用于绘制，并在通道结束时解析回目标纹理。 */
        for (const auto& render_buffer : m_FrameBufferDesc.ColorRenderBuffers)
        {
            MTL::Texture* target = AcquireRenderTargetTexture(render_buffer);

            if (target && samples > 1 && target->sampleCount() == 1)
            {
                TextureDesc attachment_desc{};
                attachment_desc.Width = width;
                attachment_desc.Height = height;
                attachment_desc.Format = render_buffer.RenderTarget->GetTextureDesc().Format;
                attachment_desc.SamplerType = render_buffer.RenderTarget->GetTextureDesc().SamplerType;
                attachment_desc.Depth = render_buffer.RenderTarget->GetTextureDesc().Depth;
                attachment_desc.Samples = samples;

                MTL::Texture* multisample = CreateAttachmentTexture(attachment_desc, samples);
                if (multisample)
                {
                    m_ColorAttachments.push_back(multisample);
                    m_ColorResolveTextures.push_back(target);
                    continue;
                }

                /* 创建失败时退回单采样绘制，至少不会渲染到空附件 */
                CORE_LOG_WARN("MetalFrameBuffer: failed to create MSAA color attachment, falling back");
            }

            if (target)
            {
                m_ColorAttachments.push_back(target);
                m_ColorResolveTextures.push_back(nullptr);
                continue;
            }

            /* 未指定 RenderTarget 时只能按描述自行创建。FrameBufferDesc 不携带颜色
             * 格式信息，使用引擎纹理的通用格式 RGBA8，与其余离屏渲染目标保持一致。 */
            TextureDesc attachment_desc{};
            attachment_desc.Width = width;
            attachment_desc.Height = height;
            attachment_desc.Format = TextureFormat::RGBA8;
            attachment_desc.SamplerType = SamplerType::Sampler2D;
            attachment_desc.Samples = samples;

            MTL::Texture* created = CreateAttachmentTexture(attachment_desc, samples);
            m_ColorAttachments.push_back(created);
            m_ColorResolveTextures.push_back(nullptr);
        }

        /* 深度（以及可能的模板）附件 */
        MTL::Texture* depth_target = AcquireRenderTargetTexture(m_FrameBufferDesc.DepthRenderBuffer);
        if (!depth_target && m_FrameBufferDesc.DepthRenderBuffer.RenderTarget)
        {
            /* 指定了 RenderTarget 但不是 Metal 纹理，说明资源类型不匹配 */
            CORE_LOG_ERROR("MetalFrameBuffer: depth render target is not a Metal texture");
        }

        if (depth_target && samples > 1 && depth_target->sampleCount() == 1)
        {
            TextureDesc attachment_desc{};
            attachment_desc.Width = width;
            attachment_desc.Height = height;
            attachment_desc.Format = m_FrameBufferDesc.DepthRenderBuffer.RenderTarget->GetTextureDesc().Format;
            attachment_desc.SamplerType = m_FrameBufferDesc.DepthRenderBuffer.RenderTarget->GetTextureDesc().SamplerType;
            attachment_desc.Depth = m_FrameBufferDesc.DepthRenderBuffer.RenderTarget->GetTextureDesc().Depth;
            attachment_desc.Samples = samples;

            m_DepthAttachment = CreateAttachmentTexture(attachment_desc, samples);
            if (m_DepthAttachment)
                m_DepthResolveTexture = depth_target;
            else
                m_DepthAttachment = depth_target;
        }
        else if (depth_target)
        {
            m_DepthAttachment = depth_target;
        }

        if (!m_DepthAttachment)
        {
            TextureDesc attachment_desc{};
            attachment_desc.Width = width;
            attachment_desc.Height = height;
            attachment_desc.Format = TextureFormat::Depth24Stencil8;
            attachment_desc.SamplerType = SamplerType::Sampler2D;
            attachment_desc.Samples = samples;
            m_DepthAttachment = CreateAttachmentTexture(attachment_desc, samples);
        }

        /* 模板附件：优先使用独立的 StencilRenderBuffer；
         * 否则当深度格式本身包含模板分量时，复用同一张纹理。 */
        MTL::Texture* stencil_target = AcquireRenderTargetTexture(m_FrameBufferDesc.StencilRenderBuffer);
        if (stencil_target)
        {
            m_StencilAttachment = stencil_target;
        }
        else if (m_FrameBufferDesc.StencilRenderBuffer.RenderTarget)
        {
            CORE_LOG_ERROR("MetalFrameBuffer: stencil render target is not a Metal texture");
        }
        else if (m_DepthAttachment && IsStencilFormat(
            m_FrameBufferDesc.DepthRenderBuffer.RenderTarget
                ? m_FrameBufferDesc.DepthRenderBuffer.RenderTarget->GetTextureDesc().Format
                : TextureFormat::Depth24Stencil8))
        {
            m_StencilAttachment = m_DepthAttachment;
            m_StencilAttachment->retain();
        }
    }

    void MetalFrameBuffer::UpdateRenderPassDescriptor()
    {
        if (!m_RenderPassDescriptor)
            return;

        for (size_t i = 0; i < m_ColorAttachments.size(); ++i)
        {
            auto* attachment = m_RenderPassDescriptor->colorAttachments()->object(static_cast<NS::UInteger>(i));
            attachment->setTexture(m_ColorAttachments[i]);
            attachment->setLoadAction(MTL::LoadActionClear);
            attachment->setClearColor(MTL::ClearColor(0.0, 0.0, 0.0, 1.0));

            MTL::Texture* resolve_target = i < m_ColorResolveTextures.size()
                ? m_ColorResolveTextures[i]
                : nullptr;

            if (resolve_target)
            {
                attachment->setStoreAction(MTL::StoreActionMultisampleResolve);
                attachment->setResolveTexture(resolve_target);
            }
            else
            {
                attachment->setStoreAction(MTL::StoreActionStore);
            }
        }

        if (m_DepthAttachment)
        {
            auto* attachment = m_RenderPassDescriptor->depthAttachment();
            attachment->setTexture(m_DepthAttachment);
            attachment->setLoadAction(MTL::LoadActionClear);
            /* Reversed-Z：远平面深度为 0，清零取 0 */
            attachment->setClearDepth(0.0);

            if (m_DepthResolveTexture)
            {
                attachment->setStoreAction(MTL::StoreActionMultisampleResolve);
                attachment->setResolveTexture(m_DepthResolveTexture);
            }
            else
            {
                attachment->setStoreAction(MTL::StoreActionStore);
            }
        }

        if (m_StencilAttachment)
        {
            auto* attachment = m_RenderPassDescriptor->stencilAttachment();
            attachment->setTexture(m_StencilAttachment);
            attachment->setLoadAction(MTL::LoadActionClear);
            attachment->setStoreAction(MTL::StoreActionStore);
            attachment->setClearStencil(0);
        }
    }

    void MetalFrameBuffer::ReleaseAttachments()
    {
        for (auto* texture : m_ColorAttachments)
        {
            if (texture)
                texture->release();
        }
        m_ColorAttachments.clear();

        for (auto* texture : m_ColorResolveTextures)
        {
            if (texture)
                texture->release();
        }
        m_ColorResolveTextures.clear();

        if (m_DepthResolveTexture)
        {
            m_DepthResolveTexture->release();
            m_DepthResolveTexture = nullptr;
        }

        if (m_DepthAttachment)
        {
            m_DepthAttachment->release();
            m_DepthAttachment = nullptr;
        }

        if (m_StencilAttachment)
        {
            m_StencilAttachment->release();
            m_StencilAttachment = nullptr;
        }

        /* 描述符仍引用旧纹理，必须先清空再重建，避免悬垂引用 */
        if (m_RenderPassDescriptor)
        {
            for (NS::UInteger i = 0; i < MAX_COLOR_ATTACHMENT_NUM; ++i)
            {
                m_RenderPassDescriptor->colorAttachments()->object(i)->setTexture(nullptr);
            }
            m_RenderPassDescriptor->depthAttachment()->setTexture(nullptr);
            m_RenderPassDescriptor->stencilAttachment()->setTexture(nullptr);
        }
    }
}

#endif /* PLATFORM_MACOS */
