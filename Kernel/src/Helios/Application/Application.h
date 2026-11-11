#pragma once
#include "Helios/VirtualDevice/DeviceWindow.h"
#include "Helios/Core/LayerStack.h"

namespace Helios
{
	class ImGuiLayer;

	class HELIOS_API Application 
	{
	public:
		Application(const std::string& window_title = "Unnamed App", uint32_t width = 1920, uint32_t height = 1080,
			bool start_maximized = false);
		virtual ~Application();

		static Application* Instance();
		DeviceWindow& GetWindow() { return *m_pWindow; }

		/* 窗口标题 */
		[[nodiscard]] const std::string& GetName() const { return m_Name; }

		/* UI 层：不装就没有 UI 阶段，层的 OnImGuiRender 整段不会被调用；
		 * 装了作为覆盖层入栈，外观与布局文件名由它的 Config 决定。 */
		void SetUILayer(const SharedPtr<ImGuiLayer>& layer);

		virtual void Run();

		/* 推进一帧。桌面的 Run 就是循环调它；平台自己的回调（iOS 的 display link、
		 * Android 的 looper）每帧调一次即可，不必进 Run。暂停期间整帧跳过。 */
		void Tick();
		void Close();

		/* 进后台停帧、回前台恢复。暂停期间既不推进逻辑也不提交绘制 ——
		 * 移动端退后台时 drawable 会被系统收回，这时再画就是崩。 */
		void SetPaused(bool paused);
		[[nodiscard]] bool IsPaused() const { return m_IsPaused; }

		/* 层序由层自己的优先级决定（见 ILayer），同优先级按调用先后 */
		void PushLayer(const SharedPtr<ILayer>& layer);
		void PushOverlay(const SharedPtr<ILayer>& layer);
		void PopLayer(const SharedPtr<ILayer>& layer);
		void PopOverlay(const SharedPtr<ILayer>& layer);

		SharedPtr<ILayer> GetLayerByName(const std::string& name) { return m_LayerStack.GetLayerByName(name); }

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
		/* 可选 UI 层（见 SetUILayer）；空 = 应用不带界面 */
		SharedPtr<ImGuiLayer> m_pUILayer{ nullptr };
		LayerStack m_LayerStack{};
		std::string m_Name{};
		bool m_IsRunning{ true };
		bool m_IsMinimized{ false };
		bool m_IsPaused{ false };
		/* 首帧出图只打一次日志：冷启动预算按这个点量 */
		bool m_FirstFrameLogged{ false };
		double m_LastFrameTime{ 0.0 };
	};

	// Implement in clients
	UniquePtr<Application> CreateApplication();

	/* 平台事件循环：移动端进系统 run loop，帧由窗口的显示链路推；
	 * 桌面的 Run 自己走 while，不经过这里。 */
	HELIOS_API void RunPlatformEventLoop();
}