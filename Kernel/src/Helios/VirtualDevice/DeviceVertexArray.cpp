#include "Pch.h"
#include "DeviceVertexArray.h"
#include "Helios/Renderer/Renderer.h"
#if HELIOS_PLATFORM_DESKTOP
#include "GraphicsAPI/OpenGL/OpenGLVertexArray.h"
#endif
#if HELIOS_PLATFORM_APPLE
#include "GraphicsAPI/Metal/MetalVertexArray.h"
#endif

namespace Helios
{
	SharedPtr<DeviceVertexArray> DeviceVertexArray::Create(const std::string& name)
	{
		switch (Renderer::CurrentAPI())
		{
		case RenderAPI::None:
			CORE_LOG_ERROR("RenderAPI can't be None!");
			return nullptr;
#if HELIOS_PLATFORM_DESKTOP
		case RenderAPI::OpenGL:
			return CreateSharedPtr<OpenGLVertexArray>(name);
#endif
#if HELIOS_PLATFORM_APPLE
		case RenderAPI::Metal:
			return CreateSharedPtr<MetalVertexArray>(name);
#endif
		default:
			CORE_LOG_ERROR("Unknown RenderAPI is unsupported!");
			return nullptr;
		}
	}
}
