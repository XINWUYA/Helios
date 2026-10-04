#include "Pch.h"
#include "Application.h"
#include <GLFW/glfw3.h>
#include "Helios/Events/ApplicationEvent.h"
#include "Helios/ImGui/ImGuiLayer.h"
#include "Helios/Renderer/Renderer.h"
#include "Helios/Renderer/RenderAPI.h"

namespace Helios 
{
	Application* Application::s_pInstance = nullptr;

	Application::Application(const std::string& window_title, uint32_t width, uint32_t height)
	{
		PROFILE_FUNCTION();

		ASSERT(!s_pInstance, "Application already exist!");
		s_pInstance = this;
		m_Name = window_title;

		m_pWindow = DeviceWindow::Create({ window_title, width, height });
		m_pWindow->SetEventCallback(BIND_EVENT_FUNC(Application::OnEvent));
		Renderer::Init();
		m_pWindow->InitGraphicsContext();

		/* 视口按物理像素建立（GetWidth/GetHeight 语义就是物理像素，与 RenderTarget 一致） */
		Renderer::SetViewport(0, 0, m_pWindow->GetWidth(), m_pWindow->GetHeight());

		m_pImGuiLayer = CreateSharedPtr<ImGuiLayer>();
		PushOverlay(m_pImGuiLayer);
	}

	Application::~Application()
	{
		PROFILE_FUNCTION();

		m_LayerStack.Release();
		Renderer::Release();
	}

	Application* Application::Instance()
	{
		return s_pInstance;
	}

	void Application::Run()
	{
		PROFILE_FUNCTION();

		while (m_IsRunning)
		{
			PROFILE_SCOPE("Running Loops");

			const float time = static_cast<float>(glfwGetTime());
			const float delta_time = time - m_LastFrameTime;
			m_LastFrameTime = time;

			/* 最小化期间冻结布局持久化：ImGui 会把宿主尺寸钳制后的 DockNode 比例写进 imgui.ini，一旦
			 * 落盘、窗口还原后就回不到原样了。只冻结 ini、不跳过 DockSpace 提交 —— DockSpace inactive
			 * 会让停靠窗口失去 parent 而脱坞。 */
			if (m_pImGuiLayer)
				m_pImGuiLayer->SetLayoutSavingEnabled(!m_IsMinimized);

			if (!m_IsMinimized)
			{
				/* 帧开始时准备本帧的默认渲染目标（显式交换链后端在此取得 drawable，
				 * 直接绘制到默认帧缓冲的后端为空实现），调用方无需区分平台。 */
				if (auto render_api = Renderer::GetRenderAPI())
				{
					render_api->PrepareNextFrame();
				}

				/* 先更新逻辑层和渲染层 */
				{
					PROFILE_SCOPE("Update Layers");

					for (const auto& layer : m_LayerStack)
						layer->OnUpdate(delta_time);
				}

				/* 后更新UI */
				m_pImGuiLayer->Begin();
				{
					PROFILE_SCOPE("Update ImGui Layers");

					for (const auto& layer : m_LayerStack)
						layer->OnImGuiRender();
				}
				m_pImGuiLayer->End();
			}

			// SwapBuffer，显示帧画面到屏幕
			m_pWindow->OnUpdate();
		}
	}

	void Application::Close()
	{
		m_IsRunning = false;
	}

	void Application::PushLayer(const SharedPtr<ILayer>& layer)
	{
		PROFILE_FUNCTION();

		m_LayerStack.PushLayer(layer);
	}

	void Application::PushOverlay(const SharedPtr<ILayer>& layer)
	{
		PROFILE_FUNCTION();

		m_LayerStack.PushOverlay(layer);
	}

	void Application::OnEvent(IEvent* event)
	{
		PROFILE_FUNCTION();

		//CORE_LOG_INFO("{0}", *event);

		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<WindowCloseEvent>(BIND_EVENT_FUNC(Application::OnHandleWindowCloseEvent));
		dispatcher.Dispatch<WindowResizeEvent>(BIND_EVENT_FUNC(Application::OnHandleWindowResizeEvent));
		dispatcher.Dispatch<WindowIconifyEvent>(BIND_EVENT_FUNC(Application::OnHandleWindowIconifyEvent));

		// 从上层Layer向下层Layer传递
		for (auto it = m_LayerStack.rbegin(); it != m_LayerStack.rend(); ++it)
		{
			if (event->Handled) // 事件停止传递
				break;
			
			(*it)->OnEvent(event);
		}
	}

	bool Application::OnHandleWindowCloseEvent(IEvent* event)
	{
		Close();
		return true;
	}

	bool Application::OnHandleWindowResizeEvent(IEvent* event)
	{
		PROFILE_FUNCTION();

		const auto resize_event = dynamic_cast<WindowResizeEvent*>(event);
		if (resize_event->GetWidth() == 0 || resize_event->GetHeight() == 0)
		{
			m_IsMinimized = true;
			return false;
		}

		m_IsMinimized = false;

		/* WindowResizeEvent 携带的是帧缓冲的物理像素（见各后端的
		 * OnFramebufferSizeChanged），与 viewport / RenderTarget 的单位一致。 */
		Renderer::SetViewport(0, 0, resize_event->GetWidth(), resize_event->GetHeight());

		// 事件继续向下层传递，应返回false
		return false;
	}

	/* 最小化/还原由 GLFW 的 iconify 回调显式上报：
	 * 最小化时帧缓冲为 0×0，但各后端都会忽略 0 尺寸（不重建 drawable / 不派发 resize），
	 * 因此不能靠"0 尺寸"推断最小化，否则两端行为不一致（Windows 派发 0×0、macOS 不派发）。 */
	bool Application::OnHandleWindowIconifyEvent(IEvent* event)
	{
		PROFILE_FUNCTION();

		const auto iconify_event = static_cast<WindowIconifyEvent*>(event);
		m_IsMinimized = iconify_event->IsIconified();

		// 事件继续向下层传递，应返回false
		return false;
	}
}