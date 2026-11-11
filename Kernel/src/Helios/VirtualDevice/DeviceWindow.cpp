#include "Pch.h"
#include "DeviceWindow.h"
#include "Helios/Renderer/Renderer.h"
#include "GraphicsAPI/OpenGL/OpenGLWindow.h"
#if HELIOS_PLATFORM_MACOS
#include "GraphicsAPI/Metal/MetalWindow.h"
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
        case RenderAPI::OpenGL:
            return CreateUniquePtr<OpenGLWindow>(desc);
#if HELIOS_PLATFORM_MACOS
        case RenderAPI::Metal:
            return CreateUniquePtr<MetalWindow>(desc);
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