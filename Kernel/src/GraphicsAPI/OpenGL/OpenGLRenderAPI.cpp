#include "Pch.h"
#include "OpenGLRenderAPI.h"
#include "OpenGLCommon.h"
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
		switch (severity)
		{
		case GL_DEBUG_SEVERITY_HIGH:         CORE_LOG_CRITICAL(message); return;
		case GL_DEBUG_SEVERITY_MEDIUM:       CORE_LOG_ERROR(message); return;
		case GL_DEBUG_SEVERITY_LOW:          CORE_LOG_WARN(message); return;
		case GL_DEBUG_SEVERITY_NOTIFICATION: CORE_LOG_INFO(message); return;
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
		glEnable(GL_SCISSOR_TEST);
		glScissor((GLint)x, (GLint)y, (GLsizei)width, (GLsizei)height);
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
