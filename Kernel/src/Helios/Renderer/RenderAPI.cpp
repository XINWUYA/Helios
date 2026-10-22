#include "Pch.h"
#include "RenderAPI.h"
#include "GraphicsAPI/OpenGL/OpenGLRenderAPI.h"
#ifdef PLATFORM_MACOS
#include "GraphicsAPI/Metal/MetalRenderAPI.h"
#endif

namespace Helios
{
#ifdef PLATFORM_MACOS
	/* macOS平台默认使用Metal */
	int RenderAPI::m_API = RenderAPI::Metal;
#else
	/* 其他平台使用OpenGL */
	int RenderAPI::m_API = RenderAPI::OpenGL;
#endif

	SharedPtr<RenderAPI> RenderAPI::Create()
	{
		switch (m_API)
		{
		case None:
			CORE_LOG_ERROR("RenderAPI can't be None!");
			return nullptr;
		case OpenGL:
			CORE_LOG_INFO("Creating OpenGL RenderAPI...");
			return CreateSharedPtr<OpenGLRenderAPI>();
#ifdef PLATFORM_MACOS
		case Metal:
			CORE_LOG_INFO("Creating Metal RenderAPI...");
			return CreateSharedPtr<MetalRenderAPI>();
#endif
		default:
			CORE_LOG_ERROR("Unknown RenderAPI is unsupported!");
			return nullptr;
		}
	}

	bool RenderAPI::TakeViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
	{
		const ViewportRegion region{ static_cast<int32_t>(x), static_cast<int32_t>(y), width, height };
		if (m_HasAppliedViewport && m_AppliedViewport == region)
			return false;

		m_AppliedViewport = region;
		m_HasAppliedViewport = true;

		/* 视口变了，"裁剪框是否等于全屏"要重判 */
		m_HasAppliedScissor = false;
		return true;
	}

	bool RenderAPI::TakeScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
	{
		const ViewportRegion region{ static_cast<int32_t>(x), static_cast<int32_t>(y), width, height };
		if (m_HasAppliedScissor && m_AppliedScissor == region)
			return false;

		m_AppliedScissor = region;
		m_HasAppliedScissor = true;
		return true;
	}

	void RenderAPI::ResetAppliedViewport()
	{
		m_HasAppliedViewport = false;
		m_HasAppliedScissor = false;
	}
}
