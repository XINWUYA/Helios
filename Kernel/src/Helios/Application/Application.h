#pragma once
#include "Helios/VirtualDevice/DeviceWindow.h"
#include "Helios/Core/LayerStack.h"
#include "Helios/ImGui/ImGuiLayer.h"

namespace Helios
{
	class Application 
	{
	public:
		/* ui_style_installer：应用自带的界面字体 / 风格安装器（缺省 = ImGui 默认外观）。
		 * 在 ImGui 上下文建立后、字体图集上传前调用一次（见 ImGuiLayer::StyleInstaller）。 */
		Application(const std::string& window_title = "Unnamed App", uint32_t width = 1920, uint32_t height = 1080,
			bool start_maximized = false, ImGuiLayer::StyleInstaller ui_style_installer = {});
		virtual ~Application();

		static Application* Instance();
		DeviceWindow& GetWindow() { return *m_pWindow; }

		/* 应用名（即窗口标题）。用于区分各应用自身的配置文件（如 ImGui 布局 ini）。 */
		[[nodiscard]] const std::string& GetName() const { return m_Name; }

		virtual void Run();
		void Close();

		void PushLayer(const SharedPtr<ILayer>& layer);
		void PushOverlay(const SharedPtr<ILayer>& layer);

		SharedPtr<ILayer> GetLayerByName(const std::string& name) { return m_LayerStack.GetLayerByName(name); }
		const SharedPtr<ImGuiLayer>& GetImGuiLayer() const { return m_pImGuiLayer; }

		virtual void OnEvent(IEvent* event);
		virtual bool OnHandleWindowCloseEvent(IEvent* event);
		virtual bool OnHandleWindowResizeEvent(IEvent* event);
		virtual bool OnHandleWindowIconifyEvent(IEvent* event);

	protected:
		static Application* s_pInstance;
		UniquePtr<DeviceWindow> m_pWindow{ nullptr };
		SharedPtr<ImGuiLayer> m_pImGuiLayer{ nullptr };
		LayerStack m_LayerStack{};
		std::string m_Name{};
		bool m_IsRunning{ true };
		bool m_IsMinimized{ false };
		float m_LastFrameTime{ 0.0f };
	};

	// Implement in clients
	UniquePtr<Application> CreateApplication();
}