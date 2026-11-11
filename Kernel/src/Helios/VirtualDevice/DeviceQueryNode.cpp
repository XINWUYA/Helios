#include "Pch.h"
#include "DeviceQueryNode.h"
#include "Helios/Renderer/Renderer.h"
#if HELIOS_PLATFORM_DESKTOP
#include "GraphicsAPI/OpenGL/OpenGLQueryNode.h"
#endif
#if HELIOS_PLATFORM_APPLE
#include "GraphicsAPI/Metal/MetalQuery.h"
#endif

namespace Helios
{
	DeviceQueryNode* DeviceQueryNode::Create()
	{
		switch (Renderer::CurrentAPI())
		{
		case RenderAPI::None:
			CORE_LOG_ERROR("RenderAPI can't be None!");
			break;
#if HELIOS_PLATFORM_DESKTOP
		case RenderAPI::OpenGL:
			return new OpenGLQueryNode;
#endif
#if HELIOS_PLATFORM_APPLE
		case RenderAPI::Metal:
			return new MetalQueryNode;
#endif
		default:
			CORE_LOG_ERROR("Unknown RenderAPI is unsupported!");
			break;
		}
		return nullptr;
	}
}