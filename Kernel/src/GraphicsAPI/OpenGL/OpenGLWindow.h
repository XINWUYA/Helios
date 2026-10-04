#pragma once
#include "Helios/VirtualDevice/DeviceWindow.h"
#include "Helios/VirtualDevice/DeviceContext.h"

struct GLFWwindow;

namespace Helios
{
	/* GLWindow类：
	 * 使用GLFWwindow创建OpenGL窗口
	 */
	class OpenGLWindow : public DeviceWindow
	{
	public:
		OpenGLWindow(const WindowDesc& desc);
		~OpenGLWindow() override;

		/* 更新，交换一帧 */
		void OnUpdate() override;

		/* 获取宽度/高度（物理像素，与创建时指定的渲染分辨率一致） */
		[[nodiscard]] uint32_t GetWidth() const override { return m_WindowInfo.Descriptor.Width; }
		[[nodiscard]] uint32_t GetHeight() const override { return m_WindowInfo.Descriptor.Height; }
		[[nodiscard]] float GetContentScale() const override { return m_WindowInfo.ContentScale; }

		/* 设置垂直同步 */
		[[nodiscard]] bool IsVSync() const override { return m_WindowInfo.Descriptor.IsVSync; }
		void SetVSync(bool enable) override;

		/* 获取GLFWwindow* */
		[[nodiscard]] void* GetNativeWindow() const override { return m_pGLFWWindow; }

		/* 设置响应事件 */
		virtual void SetEventCallback(const EventCallbackFunc& callback) override { m_WindowInfo.CallBackFunc = callback; }

	private:
		/* 创建窗口；创建上下文；绑定响应事件 */
		void Build(const WindowDesc& desc);
		/* 销毁窗口 */
		void Destroy();

		/* GLFW 回调转发 */
		void OnFramebufferSizeChanged(int width, int height);
		void OnContentScaleChanged(float scale_x, float scale_y);

		/* 窗口信息 */
		struct WindowInfo
		{
			WindowDesc Descriptor;			/* 窗口描述（物理像素） */
			float ContentScale{ 1.0f };		/* 像素 / 点 */
			class OpenGLWindow* Owner{ nullptr };
			EventCallbackFunc CallBackFunc; /* 窗口事件回调 */
		};

		/* GLFWwindow指针 */
		GLFWwindow* m_pGLFWWindow{ nullptr };
		/* 窗口信息*/
		WindowInfo m_WindowInfo{};
		/* 窗口上下文 */
		UniquePtr<DeviceContext> m_pRenderContext{ nullptr };
	};

}