#pragma once
#include "Helios/Events/Event.h"

namespace Helios
{
	/* 窗口描述：引擎只用物理像素这一套尺寸 —— Width/Height 就是目标渲染分辨率，
	 * GetWidth()/viewport/RenderTarget/交换链一律以它为准；点 / 像素的换算在窗口实现内部完成。 */
	struct WindowDesc
	{
		std::string Title;	/* 窗口标题 */
		uint32_t Width;		/* 窗口宽度（物理像素，即目标渲染分辨率） */
		uint32_t Height;	/* 窗口高度（物理像素） */
		bool IsVSync;		/* 垂直同步 */
		bool IsMaximized;	/* 创建时是否最大化 */

		WindowDesc(std::string title = "Wuya", uint32_t width = 1920, uint32_t height = 1080,
			bool is_vsync = false, bool is_maximized = false)
			: Title(std::move(title)), Width(width), Height(height), IsVSync(is_vsync), IsMaximized(is_maximized)
		{
		}
	};

	/* 窗口类，用于创建编辑器的主窗口 */
	class DeviceWindow
	{
	public:
		using EventCallbackFunc = std::function<void(IEvent*)>;

		virtual ~DeviceWindow() = default;

		/* 更新，交换一帧 */
		virtual void OnUpdate() = 0;

		/* 获取宽度/高度（物理像素，与创建时指定的渲染分辨率一致） */
		[[nodiscard]] virtual uint32_t GetWidth() const = 0;
		[[nodiscard]] virtual uint32_t GetHeight() const = 0;

		/* 每逻辑点对应的物理像素数（Retina 通常为 2.0），
		 * 仅用于把 ImGui 的逻辑点坐标换算成像素。 */
		[[nodiscard]] virtual float GetContentScale() const { return 1.0f; }

		/* 设置垂直同步 */
		[[nodiscard]] virtual bool IsVSync() const = 0;
		virtual void SetVSync(bool enable) = 0;

		/* 获取窗口实体，如通过GLFWwindow创建的，则返回GLFWwindow* */
		[[nodiscard]] virtual void* GetNativeWindow() const = 0;

	/* 设置响应事件 */
	virtual void SetEventCallback(const EventCallbackFunc& callback) = 0;

	/* 在Renderer::Init()之后初始化窗口图形上下文（如Metal需等RenderAPI创建后设置MetalLayer） */
	virtual void InitGraphicsContext() {}

	/* 根据描述创建窗口 */
	static UniquePtr<DeviceWindow> Create(const WindowDesc& desc = {});

	protected:
		DeviceWindow() = default;
	};
}
