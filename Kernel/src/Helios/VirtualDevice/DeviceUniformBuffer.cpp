#include "Pch.h"
#include "DeviceUniformBuffer.h"
#include "Helios/Renderer/Renderer.h"
#include "GraphicsAPI/OpenGL/OpenGLUniformBuffer.h"
#if HELIOS_PLATFORM_APPLE
#include "GraphicsAPI/Metal/MetalUniformBuffer.h"
#endif

namespace Helios
{
	SharedPtr<DeviceUniformBuffer> DeviceUniformBuffer::Create(uint32_t size, uint32_t binding_point)
	{
		switch (Renderer::CurrentAPI())
		{
		case RenderAPI::None:
			CORE_LOG_ERROR("RenderAPI can't be None!");
			return nullptr;
		case RenderAPI::OpenGL:
			return CreateSharedPtr<OpenGLUniformBuffer>(size, binding_point);
#if HELIOS_PLATFORM_APPLE
		case RenderAPI::Metal:
			return CreateSharedPtr<MetalUniformBuffer>(size, binding_point);
#endif
		default:
			CORE_LOG_ERROR("Unknown RenderAPI is unsupported!");
			return nullptr;
		}
	}
}
