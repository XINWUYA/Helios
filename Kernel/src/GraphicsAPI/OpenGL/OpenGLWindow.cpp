#include "Pch.h"
#include "OpenGLWindow.h"
#include <GLFW/glfw3.h>
#include "Helios/Events/ApplicationEvent.h"
#include "Helios/Events/KeyEvent.h"
#include "Helios/Events/MouseEvent.h"
#include "Helios/Renderer/Renderer.h"

namespace Helios
{
	static uint8_t s_GLFWWindowCnt = 0;

	OpenGLWindow::OpenGLWindow(const WindowDesc& desc)
	{
		PROFILE_FUNCTION();

		Build(desc);
	}

	OpenGLWindow::~OpenGLWindow()
	{
		Destroy();
	}

	/* 更新，交换一帧 */
	void OpenGLWindow::OnUpdate()
	{
		PROFILE_FUNCTION();

		glfwPollEvents();
		m_pRenderContext->SwapBuffers();
	}

	/* 设置垂直同步 */
	void OpenGLWindow::SetVSync(bool enable)
	{
		if (enable)
			glfwSwapInterval(1);
		else
			glfwSwapInterval(0);


	}

	/* 创建窗口；创建上下文；绑定响应事件 */
	void OpenGLWindow::Build(const WindowDesc& desc)
	{
		PROFILE_FUNCTION();

		m_WindowInfo.Descriptor = desc;

		if (s_GLFWWindowCnt == 0)
		{
			PROFILE_SCOPE("glfwInit()");

			// 初始化GLFW
			int success = glfwInit();

			ASSERT(success, "Failed to init GLFW!");
			glfwSetErrorCallback([](int error_code, const char* msg)
				{
					CORE_LOG_ERROR("GLFW Error: {0}: {1}", error_code, msg);
					ASSERT(false);
				});
		}

#ifdef HELIOS_DEBUG
		if (Renderer::CurrentAPI() == RenderAPI::OpenGL)
			glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
#endif

		// macOS only supports OpenGL 4.1 core profile
#ifdef __APPLE__
		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
		glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
		glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

		// 创建 GLFW 窗口（引擎只使用物理像素一套尺寸；macOS 的点/像素换算在窗口实现内部完成）
		{
			PROFILE_SCOPE("glfwCreateWindow");

			float monitor_scale_x = 1.0f;
			float monitor_scale_y = 1.0f;
			if (GLFWmonitor* monitor = glfwGetPrimaryMonitor())
				glfwGetMonitorContentScale(monitor, &monitor_scale_x, &monitor_scale_y);
			if (monitor_scale_x <= 0.0f) monitor_scale_x = 1.0f;
			if (monitor_scale_y <= 0.0f) monitor_scale_y = 1.0f;

			const int window_width = static_cast<int>(desc.Width / monitor_scale_x + 0.5f);
			const int window_height = static_cast<int>(desc.Height / monitor_scale_y + 0.5f);

			m_pGLFWWindow = glfwCreateWindow(
				window_width < 1 ? 1 : window_width,
				window_height < 1 ? 1 : window_height,
				desc.Title.c_str(), nullptr, nullptr);
			++s_GLFWWindowCnt;
		}

		m_pRenderContext = DeviceContext::Create(m_pGLFWWindow);
		m_pRenderContext->Init();

		/* 回写帧缓冲实际的物理像素尺寸（可能被窗口服务器夹取） */
		int framebuffer_width = 0;
		int framebuffer_height = 0;
		glfwGetFramebufferSize(m_pGLFWWindow, &framebuffer_width, &framebuffer_height);

		float scale_x = 1.0f;
		float scale_y = 1.0f;
		glfwGetWindowContentScale(m_pGLFWWindow, &scale_x, &scale_y);

		m_WindowInfo.Descriptor = desc;
		if (framebuffer_width > 0 && framebuffer_height > 0)
		{
			m_WindowInfo.Descriptor.Width = static_cast<uint32_t>(framebuffer_width);
			m_WindowInfo.Descriptor.Height = static_cast<uint32_t>(framebuffer_height);
		}
		if (m_WindowInfo.Descriptor.Width != desc.Width || m_WindowInfo.Descriptor.Height != desc.Height)
		{
			CORE_LOG_WARN("Requested resolution {0}x{1} pixels was clamped to {2}x{3} pixels "
				"(screen working area is smaller).", desc.Width, desc.Height,
				m_WindowInfo.Descriptor.Width, m_WindowInfo.Descriptor.Height);
		}
		m_WindowInfo.ContentScale = (scale_x > 0.0f) ? scale_x : 1.0f;
		m_WindowInfo.Owner = this;

		CORE_LOG_INFO("Create GLFW window {0}: {1}x{2} pixels, content scale {3}",
			desc.Title, m_WindowInfo.Descriptor.Width, m_WindowInfo.Descriptor.Height, m_WindowInfo.ContentScale);

		// 指定窗口信息
		glfwSetWindowUserPointer(m_pGLFWWindow, &m_WindowInfo);
		SetVSync(desc.IsVSync);

		/* 设置帧缓冲 Resize 回调（物理像素），viewport 以像素为准 */
		glfwSetFramebufferSizeCallback(m_pGLFWWindow, [](GLFWwindow* window, int width, int height)
			{
				WindowInfo* info = (WindowInfo*)glfwGetWindowUserPointer(window);
				if (!info)
					return;

				if (info->Owner)
					info->Owner->OnFramebufferSizeChanged(width, height);
			});

		// 设置内容缩放回调（窗口在不同 DPI 屏幕间移动时触发）
		glfwSetWindowContentScaleCallback(m_pGLFWWindow, [](GLFWwindow* window, float x_scale, float y_scale)
			{
				WindowInfo* info = (WindowInfo*)glfwGetWindowUserPointer(window);
				if (!info)
					return;

				if (info->Owner)
					info->Owner->OnContentScaleChanged(x_scale, y_scale);
			});

		// 设置窗口Close回调
		glfwSetWindowCloseCallback(m_pGLFWWindow, [](GLFWwindow* window)
			{
				WindowInfo& info = *(WindowInfo*)glfwGetWindowUserPointer(window);

				WindowCloseEvent event;
				info.CallBackFunc(&event);
			});

		// 设置键盘回调
		glfwSetKeyCallback(m_pGLFWWindow, [](GLFWwindow* window, int key, int scancode, int action, int mods) 
			{
				WindowInfo& info = *(WindowInfo*)glfwGetWindowUserPointer(window);

				switch (action)
				{
					case GLFW_PRESS:
					{
						KeyPressedEvent event(key, 0);
						info.CallBackFunc(&event);
						break;
					}
					case GLFW_RELEASE:
					{
						KeyReleasedEvent event(key);
						info.CallBackFunc(&event);
						break;
					}
					case GLFW_REPEAT:
					{
						KeyPressedEvent event(key, 1);
						info.CallBackFunc(&event);
						break;
					}
				}
			});

		// 设置输入文本回调
		glfwSetCharCallback(m_pGLFWWindow, [](GLFWwindow* window, unsigned int key)
			{
				WindowInfo& info = *(WindowInfo*)glfwGetWindowUserPointer(window);

				KeyTypedEvent event(key);
				info.CallBackFunc(&event);
			});

		// 设置鼠标按键回调
		glfwSetMouseButtonCallback(m_pGLFWWindow, [](GLFWwindow* window, int button, int action, int mods)
			{
				WindowInfo& info = *(WindowInfo*)glfwGetWindowUserPointer(window);

				switch (action)
				{
					case GLFW_PRESS:
					{
						MouseButtonPressedEvent event(button);
						info.CallBackFunc(&event);
						break;
					}
					case GLFW_RELEASE:
					{
						MouseButtonReleasedEvent event(button);
						info.CallBackFunc(&event);
						break;
					}
				}
			});

		// 设置鼠标滚轮回调
		glfwSetScrollCallback(m_pGLFWWindow, [](GLFWwindow* window, double xoffset, double yoffset)
			{
				WindowInfo& info = *(WindowInfo*)glfwGetWindowUserPointer(window);

				MouseScrolledEvent event((float)xoffset, (float)yoffset);
				info.CallBackFunc(&event);
			});

		// 设置鼠标光标回调
		glfwSetCursorPosCallback(m_pGLFWWindow, [](GLFWwindow* window, double xpos, double ypos)
			{
				WindowInfo& info = *(WindowInfo*)glfwGetWindowUserPointer(window);

				MouseMovedEvent event((float)xpos, (float)ypos);
				info.CallBackFunc(&event);
			});
	}

	/* 物理像素变化：派发 resize 事件（Application 用它设置 viewport） */
	void OpenGLWindow::OnFramebufferSizeChanged(int width, int height)
	{
		if (width <= 0 || height <= 0)
			return;

		m_WindowInfo.Descriptor.Width = width;
		m_WindowInfo.Descriptor.Height = height;

		WindowResizeEvent event(width, height);
		m_WindowInfo.CallBackFunc(&event);
	}

	/* 内容缩放变化 */
	void OpenGLWindow::OnContentScaleChanged(float scale_x, float scale_y)
	{
		(void)scale_y;
		m_WindowInfo.ContentScale = (scale_x > 0.0f) ? scale_x : 1.0f;
	}

	/* 销毁窗口 */
	void OpenGLWindow::Destroy()
	{
		PROFILE_FUNCTION();

		glfwDestroyWindow(m_pGLFWWindow);
		m_pGLFWWindow = nullptr;
		--s_GLFWWindowCnt;

		if (s_GLFWWindowCnt == 0)
		{
			glfwSetErrorCallback(nullptr);
			glfwTerminate();
		}
	}
}
