#include "Pch.h"
#include "OpenGLFrameBuffer.h"
#include "OpenGLCommon.h"
#include <glad/glad.h>

#include "OpenGLTexture.h"

namespace Helios
{
	namespace
	{
		bool IsSignedIntegerFormat(TextureFormat format)
		{
			switch (format)
			{
			case TextureFormat::R8I:
			case TextureFormat::R16I:
			case TextureFormat::R32I:
			case TextureFormat::RG8I:
			case TextureFormat::RG16I:
			case TextureFormat::RGB8I:
				return true;
			default:
				return false;
			}
		}

		bool IsUnsignedIntegerFormat(TextureFormat format)
		{
			switch (format)
			{
			case TextureFormat::R8UI:
			case TextureFormat::R16UI:
			case TextureFormat::R32UI:
			case TextureFormat::RG8UI:
			case TextureFormat::RG16UI:
			case TextureFormat::RGB8UI:
				return true;
			default:
				return false;
			}
		}
	}

	constexpr uint32_t MAX_FRAME_TARGET_SIZE = 8192;

	OpenGLFrameBuffer::OpenGLFrameBuffer(const std::string& name, const FrameBufferDesc& desc)
		: DeviceFrameBuffer(name, desc)
	{
		PROFILE_FUNCTION();

	/* 1. 创建FrameBuffer */
		glGenFramebuffers(1, &m_FrameBufferId);
		glBindFramebuffer(GL_FRAMEBUFFER, m_FrameBufferId);
#ifdef HELIOS_DEBUG
		glObjectLabel(GL_FRAMEBUFFER, m_FrameBufferId, -1, name.c_str());
#endif

	/* 2. 创建ColorAttachments */
		if (!!(desc.Usage & RenderBufferUsage::ColorAll))
		{
			GLenum rbos[MAX_COLOR_ATTACHMENT_NUM] = { GL_NONE };
			for (uint32_t i = 0; i < MAX_COLOR_ATTACHMENT_NUM; ++i)
			{
				if (!!(desc.Usage & GetRenderBufferUsageByIndex(i)))
				{
					AttachARenderBuffer(desc.ColorRenderBuffers[i], GL_COLOR_ATTACHMENT0 + i);
					rbos[i] = GL_COLOR_ATTACHMENT0 + i;
				}
			}
			glDrawBuffers(MAX_COLOR_ATTACHMENT_NUM, rbos);

			CHECK_GL_ERROR;
		}
		else
		{
			/* 深度-only FBO 必须显式禁用颜色读写缓冲，否则会因默认指向 COLOR_ATTACHMENT0 而不完整。 */
			glDrawBuffer(GL_NONE);
			glReadBuffer(GL_NONE);
		}

		/* 3. 创建Depth/Stencil Attachment */
		bool is_depth_stencil = false; /* Depth和Stencil绑定到同一个RenderBuffer */
		if ((desc.Usage & RenderBufferUsage::DepthStencil) == RenderBufferUsage::DepthStencil)
		{
			AttachARenderBuffer(desc.DepthRenderBuffer, GL_DEPTH_STENCIL_ATTACHMENT);
			is_depth_stencil = true;
		}
		
		if (!is_depth_stencil)
		{
			if (!!(desc.Usage & RenderBufferUsage::Depth)) /* 仅绑定Depth */
			{
				AttachARenderBuffer(desc.DepthRenderBuffer, GL_DEPTH_ATTACHMENT);
			}

			if (!!(desc.Usage & RenderBufferUsage::Stencil)) /* 仅绑定Stencil */
			{
				AttachARenderBuffer(desc.StencilRenderBuffer, GL_STENCIL_ATTACHMENT);
			}
		}

		/* 解绑 */
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		CHECK_GL_ERROR;
	}

	OpenGLFrameBuffer::~OpenGLFrameBuffer()
	{
		glDeleteFramebuffers(1, &m_FrameBufferId);
	}

	void OpenGLFrameBuffer::Bind(const FrameBufferBindInfo& bind_info)
	{
		PROFILE_FUNCTION();

		glBindFramebuffer(GL_FRAMEBUFFER, m_FrameBufferId);
		glViewport(m_FrameBufferDesc.ViewportRegion.MinX, m_FrameBufferDesc.ViewportRegion.MinY, (GLsizei)m_FrameBufferDesc.ViewportRegion.Width, (GLsizei)m_FrameBufferDesc.ViewportRegion.Height);

		/* Layered模式：将指定附件重新绑定到Texture2DArray/CubeMap的指定层/面/mip：用于CSM多Cascade渲染到同一数组纹理的不同切片，或分层渲染到Color数组纹理，或烘焙到CubeMap各面各mip */
		if (bind_info.Mode == FrameBufferBindMode::Layered)
			SetAttachmentLayer(bind_info.TargetAttachment, bind_info.ColorIndex, bind_info.LayerIndex, bind_info.MipLevel);

		const size_t clear_count = std::min(
			m_FrameBufferDesc.ColorClearValues.size(), m_FrameBufferDesc.ColorRenderBuffers.size());
		if (clear_count > 0)
		{
			const GLboolean scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
			glDisable(GL_SCISSOR_TEST);

			for (size_t i = 0; i < clear_count; ++i)
			{
				if (!m_FrameBufferDesc.ColorClearValues[i].has_value())
					continue;

				const glm::vec4& value = *m_FrameBufferDesc.ColorClearValues[i];
				const GLfloat float_values[4] = { value.r, value.g, value.b, value.a };
				const GLint int_values[4] = {
					static_cast<GLint>(value.r), static_cast<GLint>(value.g),
					static_cast<GLint>(value.b), static_cast<GLint>(value.a) };
				const GLuint uint_values[4] = {
					static_cast<GLuint>(value.r), static_cast<GLuint>(value.g),
					static_cast<GLuint>(value.b), static_cast<GLuint>(value.a) };
				const TextureFormat format = m_FrameBufferDesc.ColorRenderBuffers[i].RenderTarget->GetTextureDesc().Format;

				if (IsSignedIntegerFormat(format))
					glClearBufferiv(GL_COLOR, static_cast<GLint>(i), int_values);
				else if (IsUnsignedIntegerFormat(format))
					glClearBufferuiv(GL_COLOR, static_cast<GLint>(i), uint_values);
				else
					glClearBufferfv(GL_COLOR, static_cast<GLint>(i), float_values);
			}

			if (scissor_enabled)
				glEnable(GL_SCISSOR_TEST);
			CHECK_GL_ERROR;
		}
	}

	void OpenGLFrameBuffer::Unbind()
	{
		PROFILE_FUNCTION();

		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}

	void OpenGLFrameBuffer::Resize(uint32_t width, uint32_t height)
	{
		PROFILE_FUNCTION();

		if (width == 0 || height == 0 || width > MAX_FRAME_TARGET_SIZE || height > MAX_FRAME_TARGET_SIZE)
		{
			CORE_LOG_WARN("Attempted to resize framebuffer to {0}, {1}, but not supported!", width, height);
			return;
		}

		m_FrameBufferDesc.ViewportRegion.Width = width;
		m_FrameBufferDesc.ViewportRegion.Height = height;
	}

	/* 获取指定附件x, y像素位置的颜颜色值 */
	void OpenGLFrameBuffer::ReadPixel(uint32_t attachment_index, int x, int y, const PixelDesc& pixel_desc, void* data)
    {
		PROFILE_FUNCTION();

		ASSERT(attachment_index < m_FrameBufferDesc.ColorRenderBuffers.size());

		glBindFramebuffer(GL_FRAMEBUFFER, m_FrameBufferId);
		glReadBuffer(GL_COLOR_ATTACHMENT0 + attachment_index);

		glReadPixels(x, y, 1, 1, TranslateToOpenGLPixelFormat(pixel_desc.Format), TranslateToOpenGLPixelType(pixel_desc.Type), data);

		CHECK_GL_ERROR;
	}

	/* 指定某个ColorAttachment */
	void OpenGLFrameBuffer::ClearAttachment(uint32_t attachment_index, int level, const PixelDesc& pixel_desc, void* data)
	{
		PROFILE_FUNCTION();

		ASSERT(attachment_index < m_FrameBufferDesc.ColorRenderBuffers.size());

		const auto& render_buffer_info = m_FrameBufferDesc.ColorRenderBuffers[attachment_index];
		const auto& texture = std::dynamic_pointer_cast<OpenGLTexture>(render_buffer_info.RenderTarget);
		
#ifdef __APPLE__
		// macOS only supports OpenGL 4.1, glClearTexImage requires OpenGL 4.4+
		// Use traditional approach: bind texture to FBO and clear
		GLuint temp_fbo;
		glGenFramebuffers(1, &temp_fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, temp_fbo);
		
		GLenum texture_target = texture->m_TextureTarget;
		if (texture_target == GL_TEXTURE_CUBE_MAP)
		{
			texture_target = GL_TEXTURE_CUBE_MAP_POSITIVE_X + render_buffer_info.Layer;
		}
		
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture_target, texture->GetTextureID(), level);
		
		GLenum draw_buffers[] = { GL_COLOR_ATTACHMENT0 };
		glDrawBuffers(1, draw_buffers);
		
		// Check framebuffer status before clearing
		GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE)
		{
			CORE_LOG_ERROR("ClearAttachment: Temporary FBO incomplete, status: 0x{:X}", status);
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glDeleteFramebuffers(1, &temp_fbo);
			CHECK_GL_ERROR;
			return;
		}
		
		// Use appropriate clear function based on pixel format (integer vs float)
		GLenum gl_pixel_format = TranslateToOpenGLPixelFormat(pixel_desc.Format);
		bool is_integer_format = (gl_pixel_format == GL_RED_INTEGER || 
		                          gl_pixel_format == GL_RG_INTEGER || 
		                          gl_pixel_format == GL_RGB_INTEGER || 
		                          gl_pixel_format == GL_RGBA_INTEGER);
		
		if (is_integer_format)
		{
			GLint clear_value[4] = { 0, 0, 0, 1 };
			if (data)
			{
				memcpy(clear_value, data, sizeof(clear_value));
			}
			glClearBufferiv(GL_COLOR, 0, clear_value);
		}
		else
		{
			GLfloat clear_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
			if (data)
			{
				memcpy(clear_color, data, sizeof(clear_color));
			}
			glClearBufferfv(GL_COLOR, 0, clear_color);
		}
		
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glDeleteFramebuffers(1, &temp_fbo);
#else
		// Windows/Linux: Use modern glClearTexImage (OpenGL 4.4+)
		glClearTexImage(texture->GetTextureID(), level, TranslateToOpenGLPixelFormat(pixel_desc.Format), TranslateToOpenGLPixelType(pixel_desc.Type), data);
#endif

		CHECK_GL_ERROR;
	}
	/* 绑定一个RenderBuffer到FrameBuffer */
	void OpenGLFrameBuffer::AttachARenderBuffer(const RenderBufferInfo& render_buffer_info, GLenum attachment)
	{
		PROFILE_FUNCTION();

		/* 获取此RenderBuffer的Usage */
		RenderBufferUsage render_buffer_usage{};
		switch (attachment)
		{
		case GL_COLOR_ATTACHMENT0:
		case GL_COLOR_ATTACHMENT1:
		case GL_COLOR_ATTACHMENT2:
		case GL_COLOR_ATTACHMENT3:
		case GL_COLOR_ATTACHMENT4:
		case GL_COLOR_ATTACHMENT5:
		case GL_COLOR_ATTACHMENT6:
		case GL_COLOR_ATTACHMENT7:
			render_buffer_usage = GetRenderBufferUsageByIndex(attachment - GL_COLOR_ATTACHMENT0);
			break;
		case GL_DEPTH_ATTACHMENT:
			render_buffer_usage = RenderBufferUsage::Depth;
			break;
		case GL_STENCIL_ATTACHMENT:
			render_buffer_usage = RenderBufferUsage::Stencil;
			break;
		case GL_DEPTH_STENCIL_ATTACHMENT:
			render_buffer_usage = RenderBufferUsage::DepthStencil;
			break;
		default: 
			break;
		}

		/* 1. 获取rt的格式 */
		const auto& texture = std::dynamic_pointer_cast<OpenGLTexture>(render_buffer_info.RenderTarget);
		GLenum texture_target = GL_TEXTURE_2D;
		if (!!(texture->m_TextureDesc.Usage & TextureUsage::Sampleable)) /* 作为颜色rt */
		{
			switch (texture->m_TextureTarget)
			{
			case GL_TEXTURE_2D:
			case GL_TEXTURE_2D_MULTISAMPLE:
			case GL_TEXTURE_2D_ARRAY:
				texture_target = texture->m_TextureTarget;
				break;
			case GL_TEXTURE_CUBE_MAP:
				texture_target = GL_TEXTURE_CUBE_MAP_POSITIVE_X + render_buffer_info.Layer;
				break;
			default:
				break;
			}
		}
		else /* 作为RenderBuffer */
		{
			texture_target = GL_RENDERBUFFER;
		}

		/* 2. 把RenderBuffer绑定到FrameBuffer */
		/* 非多重采样 */
		if (texture->m_TextureDesc.Samples <= 1)
		{
			PROFILE_SCOPE("Attach RenderBuffer");
			
			/* 数组 */
			if (texture_target == GL_TEXTURE_2D_ARRAY)
			{
				glFramebufferTextureLayer(GL_FRAMEBUFFER, attachment, texture->m_TextureId, render_buffer_info.Level, render_buffer_info.Layer);
			}
			else /* GL_TEXTURE_2D/GL_TEXTURE_2D_MULTISAMPLE/GL_TEXTURE_CUBE_MAP_POSITIVE_X... */
			{
				PROFILE_SCOPE("FrameBuffer RenderBuffer");

				if(!!(texture->m_TextureDesc.Usage & TextureUsage::Sampleable))
				{
					glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, texture_target, texture->m_TextureId, render_buffer_info.Level);
				}
				else
				{
					ASSERT(texture_target == GL_RENDERBUFFER);
					glFramebufferRenderbuffer(GL_FRAMEBUFFER, attachment, texture_target, texture->m_TextureId);
				}
			}

			CHECK_GL_ERROR;
		}
		else
		{
			/* 多采样：Sampleable 资源作为 Texture 绑定；非 Sampleable 资源已在 OpenGLTexture 中创建为 RenderBuffer，直接绑定其 m_TextureId，避免重复创建 RBO 导致泄漏。 */
			if (!!(texture->m_TextureDesc.Usage & TextureUsage::Sampleable))
			{
				glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D_MULTISAMPLE, texture->m_TextureId, 0);
			}
			else
			{
				glFramebufferRenderbuffer(GL_FRAMEBUFFER, attachment, GL_RENDERBUFFER, texture->m_TextureId);
			}

			CHECK_GL_ERROR;
		}
#if 0
		if (!!(texture->m_TextureDesc.Usage & TextureUsage::Sampleable))
		{
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(texture_target, render_buffer_info.RenderTarget->GetTextureID());

			/* 限制实际使用的Mipmap Level */
			glTexParameteri(texture_target, GL_TEXTURE_BASE_LEVEL, render_buffer_info.Level);
			glTexParameteri(texture_target, GL_TEXTURE_MAX_LEVEL, render_buffer_info.Level);
		}
#endif
		CHECK_GL_FRAMEBUFFER_STATUS(GL_FRAMEBUFFER);
	}

	void OpenGLFrameBuffer::SetAttachmentLayer(FrameBufferAttachment attachment, uint16_t attachment_index, uint16_t layer, uint16_t mip_level)
	{
		PROFILE_FUNCTION();

		/* 解析目标附件的 RenderBufferInfo 与 GL attachment 枚举 */
		const RenderBufferInfo* target_info{ nullptr };
		GLenum gl_attachment{ GL_NONE };
		switch (attachment)
		{
		case FrameBufferAttachment::Depth:
			target_info = &m_FrameBufferDesc.DepthRenderBuffer;
			gl_attachment = GL_DEPTH_ATTACHMENT;
			break;
		case FrameBufferAttachment::Stencil:
			target_info = &m_FrameBufferDesc.StencilRenderBuffer;
			gl_attachment = GL_STENCIL_ATTACHMENT;
			break;
		case FrameBufferAttachment::Color:
			if (attachment_index >= m_FrameBufferDesc.ColorRenderBuffers.size())
				return;
			target_info = &m_FrameBufferDesc.ColorRenderBuffers[attachment_index];
			gl_attachment = GL_COLOR_ATTACHMENT0 + attachment_index;
			break;
		default:
			return;
		}

		if (!target_info || !target_info->RenderTarget)
			return;

		const auto& texture = std::dynamic_pointer_cast<OpenGLTexture>(target_info->RenderTarget);
		if (!texture)
			return;

		/* 按纹理目标选择层绑定方式：Texture2DArray 的 layer 是数组切片，用 glFramebufferTextureLayer；
		 * CubeMap 的 layer 是面索引（0~5），用 glFramebufferTexture2D + CUBE_MAP_POSITIVE_X + face。
		 * 典型场景：把天空盒烘焙到 CubeMap 的 6 个面、各 mip 级。 */
		if (texture->m_TextureTarget == GL_TEXTURE_2D_ARRAY)
		{
			glFramebufferTextureLayer(GL_FRAMEBUFFER, gl_attachment, texture->GetTextureID(), mip_level, layer);
		}
		else if (texture->m_TextureTarget == GL_TEXTURE_CUBE_MAP)
		{
			if (layer >= 6)
				return;
			glFramebufferTexture2D(GL_FRAMEBUFFER, gl_attachment, GL_TEXTURE_CUBE_MAP_POSITIVE_X + layer, texture->GetTextureID(), mip_level);
		}
		else
		{
			return;
		}

		CHECK_GL_FRAMEBUFFER_STATUS(GL_FRAMEBUFFER);
		CHECK_GL_ERROR;
	}
}
