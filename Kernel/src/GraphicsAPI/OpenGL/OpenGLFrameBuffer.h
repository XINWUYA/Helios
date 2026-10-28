#pragma once
#include <glad/glad.h>
#include "Helios/VirtualDevice/DeviceFrameBuffer.h"

namespace Helios
{
	/* FrameBuffer类 */
	class OpenGLFrameBuffer : public DeviceFrameBuffer
	{
	public:
		OpenGLFrameBuffer(const std::string& name, const FrameBufferDesc& desc);
		~OpenGLFrameBuffer() override;

		/* 绑定：bind_info可指定渲染到某一层 */
		void Bind(const FrameBufferBindInfo& bind_info = {}) override;
		void Unbind() override;

		void Resize(uint32_t width, uint32_t height) override;

		/* 获取指定附件x, y像素位置的颜颜色值 */
		void ReadPixel(uint32_t attachment_index, int x, int y, const PixelDesc& pixel_desc, void* data) override;
		/* 指定某个ColorAttachment */
        void ClearAttachment(uint32_t attachment_index, int level, const PixelDesc& pixel_desc, void* data) override;

	private:
		/* 绑定一个RenderBuffer到FrameBuffer */
		void AttachARenderBuffer(const RenderBufferInfo& render_buffer_info, GLenum attachment);

		/* 将指定附件重新绑定到Texture2DArray/CubeMap的指定层和mip：attachment指定目标附件类型，attachment_index为Color附件下标 */
		void SetAttachmentLayer(FrameBufferAttachment attachment, uint16_t attachment_index, uint16_t layer, uint16_t mip_level);

		/* 多重采样：临时附件建在 RBO 上，Unbind 时解析回描述里的单采样目标 */
		void CreateMultisampleAttachments(const std::string& name);
		void ResolveMultisample();

		uint32_t m_FrameBufferId{ 0 };
		uint32_t m_ResolveFrameBufferId{ 0 };
		std::vector<GLuint> m_MultisampleRenderbuffers;
	};
}
