#pragma once

#ifdef PLATFORM_MACOS

#include "Helios/VirtualDevice/DeviceFrameBuffer.h"
#include "Helios/VirtualDevice/DeviceTexture.h"
#include <Metal/Metal.hpp>
#include <vector>

namespace Helios
{
    /* Metal 的 FrameBuffer 用 RenderPassDescriptor 表达：开始渲染通道即绑定、结束即解绑。附件
     * 优先取自 RenderTarget，缺失时按描述和采样数自动创建；统一记进附件列表、按层 / 面定位。 */
    class MetalFrameBuffer : public DeviceFrameBuffer
    {
    public:
        MetalFrameBuffer(const std::string& name, const FrameBufferDesc& desc);
        ~MetalFrameBuffer() override;

        void Bind(const FrameBufferBindInfo& bind_info = {}) override;
        void Unbind() override;

        void Resize(uint32_t width, uint32_t height) override;

        void ReadPixel(uint32_t attachment_index, int x, int y, const PixelDesc& pixel_desc, void* data) override;
        void ClearAttachment(uint32_t attachment_index, int level, const PixelDesc& pixel_desc, void* data) override;

        /* Metal 特有接口 */
        MTL::RenderPassDescriptor* GetRenderPassDescriptor() const { return m_RenderPassDescriptor; }
        MTL::Texture* GetColorAttachment(uint32_t index) const;
        MTL::Texture* GetDepthAttachment() const { return m_DepthAttachment; }

    private:
        void CreateAttachments();
        void UpdateRenderPassDescriptor();
        void ReleaseAttachments();

        /* 按 FrameBufferBindInfo 定位附件到指定 mip/layer；
         * Metal 的附件 level/slice 直接对应 2DArray 的切片与 CubeMap 的 face。 */
        void ApplyAttachmentLocation(const FrameBufferBindInfo& bind_info);

        /* 取出 RenderTarget 对应的 MTL::Texture（已 retain）；不存在时返回空 */
        MTL::Texture* AcquireRenderTargetTexture(const RenderBufferInfo& render_buffer);

        /* 创建（可能为多重采样的）附件纹理，并按需登记解析目标 */
        MTL::Texture* CreateAttachmentTexture(const TextureDesc& desc, uint32_t samples);

        MTL::RenderPassDescriptor* m_RenderPassDescriptor{ nullptr };

        /* 实际参与绘制的附件纹理。借用自 RenderTarget 的会 retain，
         * 由本类新建的 newTexture 本身即持有引用，两者在析构时统一 release。 */
        std::vector<MTL::Texture*> m_ColorAttachments;
        MTL::Texture* m_DepthAttachment{ nullptr };
        MTL::Texture* m_StencilAttachment{ nullptr };

        /* MSAA 解析目标：绘制发生在多重采样附件上，通道结束时解析到这些单采样纹理 */
        std::vector<MTL::Texture*> m_ColorResolveTextures;
        MTL::Texture* m_DepthResolveTexture{ nullptr };
    };
}

#endif /* PLATFORM_MACOS */
