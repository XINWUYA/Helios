#pragma once
#include "Helios/VirtualDevice/DeviceWindow.h"
#include "Helios/Core/LayerStack.h"

namespace Helios
{
	class ImGuiLayer;

	class Application 
	{
	public:
		Application(const std::string& window_title = "Unnamed App", uint32_t width = 1920, uint32_t height = 1080);
		virtual ~Application();

		static Application* Instance();
		DeviceWindow& GetWindow() { return *m_pWindow; }

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
		bool m_IsRunning{ true };
		bool m_IsMinimized{ false };
		float m_LastFrameTime{ 0.0f };
	};

	// Implement in clients
	UniquePtr<Application> CreateApplication();
}