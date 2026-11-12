#pragma once
#include <glm/glm.hpp>
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

		/* 改窗口标题（换项目后要把项目名带上） */
		virtual void SetWindowTitle(const std::string& title) = 0;

		/* 获取窗口实体，如通过GLFWwindow创建的，则返回GLFWwindow* */
		[[nodiscard]] virtual void* GetNativeWindow() const = 0;

	/* 设置响应事件 */
	virtual void SetEventCallback(const EventCallbackFunc& callback) = 0;

	/* ---- 输入与时钟 ----
	 * 各平台窗口在这里回自己那套：桌面转 GLFW，移动端转触摸状态表。
	 * Input 与主循环只认这几个接口，不认具体平台。 */

	/* 按键是否按下（KeyCode 取值；移动端没有物理键盘时恒为 false） */
	[[nodiscard]] virtual bool QueryKeyPressed(int key) const { (void)key; return false; }
	/* 指针按键是否按下（MouseCode 取值；移动端把第一根手指映射到左键） */
	[[nodiscard]] virtual bool QueryPointerButtonPressed(int button) const { (void)button; return false; }
	/* 指针位置（窗口坐标，左上角原点，逻辑点） */
	[[nodiscard]] virtual glm::vec2 QueryPointerPosition() const { return glm::vec2(0.0f); }
	/* 窗口是否拿着系统焦点：没焦点时脚本侧输入一律不放行 */
	[[nodiscard]] virtual bool HasFocus() const { return true; }
	/* ---- 多指：一根手指点 UI，另一根转视角 ----
	 * 触摸落下时问这个回调，落在画布控件上的那根归 UI，其余归指针（脚本拿去转视角）。
	 * 坐标是逻辑点。桌面没有触摸，窗口不会调它。 */
	using UITouchTest = std::function<bool(const glm::vec2&)>;
	virtual void SetUITouchTest(UITouchTest test) { (void)test; }

	/* 给 UI 的那一路指针（窗口坐标，逻辑点）。桌面与 QueryPointer* 同源，
	 * 移动端是认领了 UI 的那根手指 —— 两根手指各喂各的，互不抢 */
	[[nodiscard]] virtual glm::vec2 QueryUIPointerPosition() const { return QueryPointerPosition(); }
	[[nodiscard]] virtual bool QueryUIPointerButtonPressed(int button) const
	{
		return QueryPointerButtonPressed(button);
	}
	/* 单调时钟（秒）。帧间隔与动画按它算，各平台同一口径 */
	[[nodiscard]] double GetTimeSeconds() const;

	/* 系统剪贴板。返回 false 表示这平台没有可用的系统剪贴板，
	 * 调用方（例如运行时 UI）回落到进程内实现。 */
	[[nodiscard]] virtual bool SetClipboardText(const std::string& text) { (void)text; return false; }
	[[nodiscard]] virtual bool GetClipboardText(std::string& text) { (void)text; return false; }

	/* 在Renderer::Init()之后初始化窗口图形上下文（如Metal需等RenderAPI创建后设置MetalLayer） */
	virtual void InitGraphicsContext() {}

	/* 根据描述创建窗口 */
	static UniquePtr<DeviceWindow> Create(const WindowDesc& desc = {});

	protected:
		DeviceWindow() = default;
	};
}
