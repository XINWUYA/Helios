#include "Pch.h"
#include "DeviceQueryBuffer.h"
#include "Helios/Renderer/Renderer.h"
#ifdef PLATFORM_MACOS
#include "GraphicsAPI/Metal/MetalQuery.h"
#endif

namespace Helios
{
	DeviceQueryBuffer* DeviceQueryBuffer::Create()
	{
		switch (Renderer::CurrentAPI())
		{
		case RenderAPI::None:
			CORE_LOG_ERROR("RenderAPI can't be None!");
			break;
		case RenderAPI::OpenGL:
			/* OpenGL 的时间戳查询与渲染通道无关（见 OpenGLQueryNode），不需要通道采样 */
			break;
#ifdef PLATFORM_MACOS
		case RenderAPI::Metal:
			return new MetalQueryBuffer;
#endif
		default:
			CORE_LOG_ERROR("Unknown RenderAPI is unsupported!");
			break;
		}
		return nullptr;
	}
}
