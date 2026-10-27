#pragma once
#include "Helios/VirtualDevice/DeviceWindow.h"
#include "Helios/Core/LayerStack.h"
#include "Helios/ImGui/ImGuiLayer.h"

namespace Helios
{
	class HELIOS_API Application 
	{
	public:
		/* ui_style_installer：应用自带的界面字体 / 风格安装器（缺省 = ImGui 默认外观）。
		 * 在 ImGui 上下文建立后、字体图集上传前调用一次（见 ImGuiLayer::StyleInstaller）。
		 * config_name：配置文件主干名（ImGui 布局 ini 之类），空 = 主应用直接用 imgui.ini；
		 * 同一项目下并行开多个应用才需要给名字，否则它们会抢同一份停靠数据。 */
		Application(const std::string& window_title = "Unnamed App", uint32_t width = 1920, uint32_t height = 1080,
			bool start_maximized = false, ImGuiLayer::StyleInstaller ui_style_installer = {},
			const std::string& config_name = {});
		virtual ~Application();

		static Application* Instance();
		DeviceWindow& GetWindow() { return *m_pWindow; }

		/* 窗口标题 */
		[[nodiscard]] const std::string& GetName() const { return m_Name; }
		/* 配置文件主干名（空 = 主应用） */
		[[nodiscard]] const std::string& GetConfigName() const { return m_ConfigName; }

		virtual void Run();
		void Close();

		void PushLayer(const SharedPtr<ILayer>& layer);
		void PushOverlay(const SharedPtr<ILayer>& layer);
		void PopLayer(const SharedPtr<ILayer>& layer);
		void PopOverlay(const SharedPtr<ILayer>& layer);

		SharedPtr<ILayer> GetLayerByName(const std::string& name) { return m_LayerStack.GetLayerByName(name); }
		const SharedPtr<ImGuiLayer>& GetImGuiLayer() const { return m_pImGuiLayer; }

		virtual void OnEvent(IEvent* event);
		virtual bool OnHandleWindowCloseEvent(IEvent* event);
		virtual bool OnHandleWindowResizeEvent(IEvent* event);
		virtual bool OnHandleWindowIconifyEvent(IEvent* event);
		virtual bool OnHandleContentScaleChangedEvent(IEvent* event);

		/* 帧末钩子：层在 OnUpdate / OnImGuiRender 里不能直接增删层（正在遍历层列表），
		 * 要换层就记下请求、在这里落地。 */
		virtual void OnFrameEnd() {}

	protected:
		static Application* s_pInstance;
		UniquePtr<DeviceWindow> m_pWindow{ nullptr };
		SharedPtr<ImGuiLayer> m_pImGuiLayer{ nullptr };
		LayerStack m_LayerStack{};
		std::string m_Name{};
		std::string m_ConfigName{};
		bool m_IsRunning{ true };
		bool m_IsMinimized{ false };
		float m_LastFrameTime{ 0.0f };
	};

	// Implement in clients
	UniquePtr<Application> CreateApplication();
}