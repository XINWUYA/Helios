#include "Pch.h"
#include "DeviceWindow.h"
#include "Helios/Renderer/Renderer.h"
#if HELIOS_PLATFORM_DESKTOP
#include "GraphicsAPI/OpenGL/OpenGLWindow.h"
#endif
#if HELIOS_PLATFORM_MACOS
#include "GraphicsAPI/Metal/MetalWindow.h"
#endif
#if HELIOS_PLATFORM_IOS
#include "GraphicsAPI/Metal/MetalWindowIOS.h"
#endif

namespace Helios
{
    /* 创建窗口 */
    UniquePtr<DeviceWindow> DeviceWindow::Create(const WindowDesc& desc)
    {
        switch (Renderer::CurrentAPI())
        {
        case RenderAPI::None:
            CORE_LOG_ERROR("RenderAPI can't be None!");
            return nullptr;
#if HELIOS_PLATFORM_DESKTOP
        case RenderAPI::OpenGL:
            return CreateUniquePtr<OpenGLWindow>(desc);
#endif
#if HELIOS_PLATFORM_MACOS
        case RenderAPI::Metal:
            return CreateUniquePtr<MetalWindow>(desc);
#elif HELIOS_PLATFORM_IOS
        case RenderAPI::Metal:
            return CreateUniquePtr<IOSWindow>(desc);
#endif
        default:
            CORE_LOG_ERROR("Unknown RenderAPI is unsupported!");
            return nullptr;
        }
    }

    double DeviceWindow::GetTimeSeconds() const
    {
        /* 单调时钟，只做差用：首次调用时刻当零点，不依赖系统墙钟 */
        static const auto start = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }
}
