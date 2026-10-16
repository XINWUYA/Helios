#include "Pch.h"
#include "OpenGLRenderAPI.h"
#include "OpenGLCommon.h"
#include "OpenGLShader.h"
#include "OpenGLTexture.h"
#include <Helios/VirtualDevice/DeviceTexture.h>
#include <Helios/Renderer/RenderQuery.h>
#include <glad/glad.h>

namespace Helios
{
	void OpenGLMessageCallback(
		unsigned source,
		unsigned type,
		unsigned id,
		unsigned severity,
		int length,
		const char* message,
		const void* userParam)
	{
		/* 不按消息 id 过滤，驱动性能警告是真实信号（如着色器被按 GL 状态重编译）；
		 * 消息只带 program 句柄，用登记表把 shader 名补在后面，便于定位 */
		const std::string annotated = std::string(message != nullptr ? message : "")
			+ OpenGLShader::AnnotateProgramHandlesInMessage(message);

		switch (severity)
		{
		case GL_DEBUG_SEVERITY_HIGH:         CORE_LOG_CRITICAL("{}", annotated); return;
		case GL_DEBUG_SEVERITY_MEDIUM:       CORE_LOG_ERROR("{}", annotated); return;
		case GL_DEBUG_SEVERITY_LOW:          CORE_LOG_WARN("{}", annotated); return;
		case GL_DEBUG_SEVERITY_NOTIFICATION: CORE_LOG_INFO("{}", annotated); return;
		default:
			CORE_LOG_ERROR("Unknown severity level!");
		}
	}

	void OpenGLRenderAPI::Init()
	{
		PROFILE_FUNCTION();

#ifdef HELIOS_DEBUG
#ifndef __APPLE__
		// glDebugMessageCallback requires OpenGL 4.3+, macOS only supports 4.1
		glEnable(GL_DEBUG_OUTPUT);
		glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
		glDebugMessageCallback(OpenGLMessageCallback, nullptr);
		glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION, 0, nullptr, GL_FALSE);
#endif
#endif

		// Blend
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		// Depth
		glEnable(GL_DEPTH_TEST);

		InitOpenGLExtensions();
	}

	void OpenGLRenderAPI::SetViewport(uint32_t x_start, uint32_t y_start, uint32_t width, uint32_t height)
	{
		glViewport((GLint)x_start, (GLint)y_start, (GLsizei)width, (GLsizei)height);
	}

	void OpenGLRenderAPI::SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
	{
		glScissor((GLint)x, (GLint)y, (GLsizei)width, (GLsizei)height);

		/* 范围等于整个 viewport 时视为全屏绘制，直接关闭裁剪（与帧缓冲绑定时一致） */
		GLint vp[4] = { 0, 0, 0, 0 };
		glGetIntegerv(GL_VIEWPORT, vp);
		if (x == static_cast<uint32_t>(vp[0]) && y == static_cast<uint32_t>(vp[1]) &&
			width == static_cast<uint32_t>(vp[2]) && height == static_cast<uint32_t>(vp[3]))
			glDisable(GL_SCISSOR_TEST);
		else
			glEnable(GL_SCISSOR_TEST);
	}

	void OpenGLRenderAPI::SetClearColor(const glm::vec4& color)
	{
		glClearColor(color.r, color.g, color.b, color.a);
	}

	void OpenGLRenderAPI::Clear()
	{
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	}

	/* 应用光栅化状态 */
	void OpenGLRenderAPI::ApplyRasterState(RenderRasterState raster_state)
	{
		PROFILE_FUNCTION();

		/* 设置CullMode */
		switch (raster_state.CullMode)
		{
		case CullMode::Cull_None:
			glDisable(GL_CULL_FACE);
			break;
		case CullMode::Cull_Front:
			glEnable(GL_CULL_FACE);
			glCullFace(GL_FRONT);
			break;
		case CullMode::Cull_Back:
			glEnable(GL_CULL_FACE);
			glCullFace(GL_BACK);
			break;
		case CullMode::Cull_Front_And_Back:
			glEnable(GL_CULL_FACE);
			glCullFace(GL_FRONT_AND_BACK);
			break;
		}

		/* 设置正面顶点绕序方式 */
		switch (raster_state.FrontFaceType)
		{
		case FrontFaceType::CW:
			glFrontFace(GL_CW);
			break;
		case FrontFaceType::CCW:
			glFrontFace(GL_CCW);
			break;
		}

		/* 设置混合模式 */
		if (raster_state.EnableBlend)
		{
			glEnable(GL_BLEND);
			glBlendEquationSeparate(
				TranslateToOpenGLBlendEquation(raster_state.BlendEquationRGB),
				TranslateToOpenGLBlendEquation(raster_state.BlendEquationA));
			glBlendFuncSeparate(
				TranslateToOpenGLBlendFunc(raster_state.BlendFuncSrcRGB), 
				TranslateToOpenGLBlendFunc(raster_state.BlendFuncDstRGB), 
				TranslateToOpenGLBlendFunc(raster_state.BlendFuncSrcA), 
				TranslateToOpenGLBlendFunc(raster_state.BlendFuncDstA));
		}
		else
		{
			glDisable(GL_BLEND);
		}

		/* 设置深度测试 */
		if (raster_state.EnableDepthWrite)
		{
			glEnable(GL_DEPTH_TEST);
			glDepthFunc(TranslateToOpenGLCompareFunc(raster_state.DepthCompareFunc));
			glDepthMask(GL_TRUE);
		}
		else
		{
			glDisable(GL_DEPTH_TEST);
		}

		/* 设置颜色Mask */
		glColorMask((GLboolean)raster_state.EnableColorWrite, (GLboolean)raster_state.EnableColorWrite, (GLboolean)raster_state.EnableColorWrite, (GLboolean)raster_state.EnableColorWrite);

	}

	void OpenGLRenderAPI::DrawIndexed(PrimitiveType type, const SharedPtr<DeviceVertexArray>& vertex_array, uint32_t index_count, uint32_t index_offset)
	{
		PROFILE_FUNCTION();

		const uint32_t count = index_count ? index_count : vertex_array->GetVertexCount();

		/* 根据索引缓冲区的类型选择对应的GL枚举和字节偏移 */
		GLenum index_gl_type = GL_UNSIGNED_INT;
		size_t index_size = sizeof(uint32_t);
		const auto& index_buffer = vertex_array->GetIndexBuffer();
		if (index_buffer && index_buffer->GetIndexType() == IndexType::UInt16)
		{
			index_gl_type = GL_UNSIGNED_SHORT;
			index_size = sizeof(uint16_t);
		}

		glDrawElements(TranslateToOpenGLPrimitiveType(type), (GLsizei)count, index_gl_type, reinterpret_cast<const void*>(index_offset * index_size));
	}

	void OpenGLRenderAPI::DrawArrays(PrimitiveType type, const SharedPtr<DeviceVertexArray>& vertex_array)
	{
		PROFILE_FUNCTION();

		const auto count = vertex_array->GetVertexCount();
		glDrawArrays(TranslateToOpenGLPrimitiveType(type), 0, count);
	}

	void OpenGLRenderAPI::Flush()
	{
		PROFILE_FUNCTION();

		glFlush();
	}

	void OpenGLRenderAPI::GenerateMipmap(const SharedPtr<DeviceTexture>& texture)
	{
		PROFILE_FUNCTION();

		/* OpenGL 的命令是立即执行的，烘焙后的绘制已经生效，直接生成即可 */
		if (texture)
			texture->GenerateMipmap();
	}

	SharedPtr<DeviceTexture> OpenGLRenderAPI::AcquireDefaultTargetSnapshot(bool& top_down)
	{
		PROFILE_FUNCTION();

		/* 默认目标的尺寸 / 区域 = 当前 GL 视口：渲染到默认目标的 Pass 已把视口
		 * 设成它的目标区域（标准情形即整窗）。没有任何 Pass 设过视口时取当前值，
		 * 尺寸为 0 则视为无可用目标、静默跳过。 */
		GLint viewport[4] = { 0, 0, 0, 0 };
		glGetIntegerv(GL_VIEWPORT, viewport);
		if (viewport[2] <= 0 || viewport[3] <= 0)
			return nullptr;

		const uint32_t width = static_cast<uint32_t>(viewport[2]);
		const uint32_t height = static_cast<uint32_t>(viewport[3]);

		/* 预览纹理跨帧复用；尺寸变化（窗口缩放）时重建 */
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

		/* 回读默认帧缓冲（画上去的是 BACK）当时刻的内容 —— 命令立即执行，
		 * 此刻拿到的正是"本 Pass 刚画完"的当刻内容；行 0 = 屏幕底行
		 * （glReadPixels 口径），与引擎离屏纹理同为底行在前 → top_down = false */
		std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
		glReadBuffer(GL_BACK);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(viewport[0], viewport[1], static_cast<GLsizei>(width),
			static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

		PixelDesc pixel_desc;
		pixel_desc.Format = PixelFormat::RGBA;
		pixel_desc.Type = PixelType::UnsignedByte;
		m_DefaultTargetSnapshot->SetData(pixels.data(), pixel_desc);

		top_down = false;
		return m_DefaultTargetSnapshot;
	}

	/* 把源纹理 mip 0 的第 src_layer 层拷到目标纹理（逐 Pass 抓取 Attachment → 常驻预览）。
	 * 走"临时 FBO + glBlitFramebuffer"：不做 4.3+ 的 glCopyImageSubData 依赖（同一份代码
	 * 要在 4.1 上下文上可用，理由与 ClearAttachment 的 __APPLE__ 分支相同）。源挂读 FBO
	 * （2D 数组用 glFramebufferTextureLayer 指定层）、目标挂画 FBO，同尺寸同格式直拷；
	 * 深度格式按 GL_DEPTH_BUFFER_BIT 走（规范要求 NEAREST），且两个 FBO 的读写缓冲都置
	 * NONE（"只有深度附件"的 FBO 在部分驱动上要这样才判定完整）。本函数在 Pass 中间被
	 * 调用，调用点的 FBO 绑定先保存、结束后还原 —— 不能把别人的绑定改掉。 */
	void OpenGLRenderAPI::CopyTexture(const SharedPtr<DeviceTexture>& src, const SharedPtr<DeviceTexture>& dst,
		uint32_t src_layer)
	{
		PROFILE_FUNCTION();

		const auto open_gl_src = std::dynamic_pointer_cast<OpenGLTexture>(src);
		const auto open_gl_dst = std::dynamic_pointer_cast<OpenGLTexture>(dst);
		if (!open_gl_src || !open_gl_dst || open_gl_src->GetTextureID() == 0 || open_gl_dst->GetTextureID() == 0)
			return;

		const TextureDesc& src_desc = src->GetTextureDesc();
		if (src_layer >= std::max(1u, src_desc.Depth))
		{
			CORE_LOG_ERROR("OpenGLRenderAPI::CopyTexture: layer {} out of range ({})",
				src_layer, src_desc.Depth);
			return;
		}

		const bool is_depth = IsDepthFormat(src_desc.Format);
		const GLenum attachment = is_depth ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0;
		const GLbitfield blit_mask = is_depth ? GL_DEPTH_BUFFER_BIT : GL_COLOR_BUFFER_BIT;

		GLint previous_read_fbo = 0;
		GLint previous_draw_fbo = 0;
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read_fbo);
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw_fbo);

		GLuint read_fbo = 0;
		GLuint draw_fbo = 0;
		glGenFramebuffers(1, &read_fbo);
		glGenFramebuffers(1, &draw_fbo);

		glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);

		/* 源和目标都可能是纹理或 RenderBuffer；纹理再按采样类型挂到 2D、单层或立方体面 */
		if (open_gl_src->IsRenderBuffer())
		{
			glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, attachment, GL_RENDERBUFFER, open_gl_src->GetTextureID());
		}
		else
		{
			switch (src_desc.SamplerType)
			{
			case SamplerType::Sampler2DArray:
			case SamplerType::Sampler3D:
				glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, attachment, open_gl_src->GetTextureID(), 0, static_cast<GLint>(src_layer));
				break;
			case SamplerType::SamplerCubeMap:
				glFramebufferTexture2D(GL_READ_FRAMEBUFFER, attachment, GL_TEXTURE_CUBE_MAP_POSITIVE_X + src_layer, open_gl_src->GetTextureID(), 0);
				break;
			default:
				glFramebufferTexture2D(GL_READ_FRAMEBUFFER, attachment, GL_TEXTURE_2D, open_gl_src->GetTextureID(), 0);
				break;
			}
		}

		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
		if (open_gl_dst->IsRenderBuffer())
		{
			glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, attachment, GL_RENDERBUFFER, open_gl_dst->GetTextureID());
		}
		else
		{
			glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, attachment, GL_TEXTURE_2D, open_gl_dst->GetTextureID(), 0);
		}

		if (is_depth)
		{
			glReadBuffer(GL_NONE);
			glDrawBuffer(GL_NONE);
		}
		else
		{
			glReadBuffer(GL_COLOR_ATTACHMENT0);
			glDrawBuffer(GL_COLOR_ATTACHMENT0);
		}

		const GLenum read_status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
		const GLenum draw_status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
		if (read_status == GL_FRAMEBUFFER_COMPLETE && draw_status == GL_FRAMEBUFFER_COMPLETE)
		{
			glBlitFramebuffer(0, 0, static_cast<GLint>(src_desc.Width), static_cast<GLint>(src_desc.Height),
				0, 0, static_cast<GLint>(src_desc.Width), static_cast<GLint>(src_desc.Height),
				blit_mask, GL_NEAREST);
		}
		else
		{
			/* 附件不完整时把源资源信息一并打出，便于定位是哪路资源 */
			CORE_LOG_ERROR("OpenGLRenderAPI::CopyTexture: framebuffer incomplete (read 0x{:X}, draw 0x{:X})"
				" src='{}' ({}x{}, sampler {}, format {}, mips {}, layer {})",
				read_status, draw_status,
				src->GetDebugName(), src_desc.Width, src_desc.Height,
				static_cast<int>(src_desc.SamplerType), static_cast<int>(src_desc.Format),
				src_desc.MipLevels, src_layer);
		}

		glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous_read_fbo));
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previous_draw_fbo));
		glDeleteFramebuffers(1, &read_fbo);
		glDeleteFramebuffers(1, &draw_fbo);

		CHECK_GL_ERROR;
	}

	/* Debug 组同时驱动 GPU 耗时作用域：组名是抓帧分组与统计面板共用的唯一标签源。
	 * 作用域无条件配对，不依赖 KHR_debug 是否可用（作用域树与抓帧组解耦）。 */
	void OpenGLRenderAPI::PushDebugGroup(const char* name)
	{
		PROFILE_FUNCTION();

		if (name)
			RenderQueryProfiler::Instance().BeginGPUScope(name);

		if (m_SupportedExtensions.KHR_debug)
			glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, name);
	}

	void OpenGLRenderAPI::PopDebugGroup()
	{
		PROFILE_FUNCTION();

		RenderQueryProfiler::Instance().EndGPUScope();

		if (m_SupportedExtensions.KHR_debug)
			glPopDebugGroup();
	}

	/* 获取OpenGL支持的扩展 */
	void OpenGLRenderAPI::InitOpenGLExtensions()
	{
		PROFILE_FUNCTION();

		/* 获取扩展数 */
		GLint num = 0;
		glGetIntegerv(GL_NUM_EXTENSIONS, &num);
		/* 收集扩展 */
		std::unordered_set<std::string_view> extensions;
		for (auto i = 0; i < num; ++i)
			extensions.emplace((const char*)glGetStringi(GL_EXTENSIONS, (GLuint)i));

#ifdef HELIOS_DEBUG
		CORE_LOG_INFO("Supported Extensions:");
		for (auto& ext : extensions)
			CORE_LOG_INFO("- {}", ext);
#endif

		m_SupportedExtensions.KHR_debug = extensions.find(std::string_view("GL_KHR_debug")) != extensions.end();
		m_SupportedExtensions.EXT_shader_framebuffer_fetch = extensions.find(std::string_view("GL_EXT_shader_framebuffer_fetch")) != extensions.end();
		m_SupportedExtensions.EXT_texture_filter_anisotropic = extensions.find(std::string_view("GL_EXT_texture_filter_anisotropic")) != extensions.end();
		m_SupportedExtensions.EXT_texture_sRGB = extensions.find(std::string_view("GL_EXT_texture_sRGB")) != extensions.end();
	}
}
