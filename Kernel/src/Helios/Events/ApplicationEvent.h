#pragma once
#include <vector>
#include "Event.h"

namespace Helios
{
	/*
	* 应用事件类
	*/
	class AppEvent : public IEvent
	{
	public:
		virtual int GetCategoryFlags() const override { return EventCategoryApplication; }

	protected:
		AppEvent() = default;
	};

	/*
	* 窗口关闭事件类
	*/
	class WindowCloseEvent : public AppEvent
	{
	public:
		EVENT_CLASS_TYPE(WindowClose)

			WindowCloseEvent() = default;
	};

	/*
	* 窗口Resize事件类
	*/
	class WindowResizeEvent : public AppEvent
	{
	public:
		EVENT_CLASS_TYPE(WindowResize)

			WindowResizeEvent(unsigned int width, unsigned int height)
			: m_Width(width), m_Height(height)
		{
		}

		unsigned int GetWidth() const { return m_Width; }
		unsigned int GetHeight() const { return m_Height; }

		std::string ToString() const override
		{
			std::stringstream ss;
			ss << "WindowResizeEvent: " << m_Width << ", " << m_Height;
			return ss.str();
		}

	private:
		unsigned int m_Width, m_Height;
	};

	/*
	* 窗口聚焦事件类 todo:
	*/
	class WindowFocusEvent : public AppEvent
	{
	public:
		EVENT_CLASS_TYPE(WindowFocus)

			WindowFocusEvent() = default;
	};

	/* 窗口最小化 / 还原事件：最小化状态由 GLFW 的 iconify 回调显式上报，不靠帧缓冲尺寸推断
	 * （最小化时帧缓冲是 0×0，但各平台是否派发这个尺寸并不一致）。 */
	class WindowIconifyEvent : public AppEvent
	{
	public:
		EVENT_CLASS_TYPE(WindowIconify)

			WindowIconifyEvent(bool iconified)
			: m_Iconified(iconified)
		{
		}

		bool IsIconified() const { return m_Iconified; }

		std::string ToString() const override
		{
			std::stringstream ss;
			ss << "WindowIconifyEvent: " << (m_Iconified ? "iconified" : "restored");
			return ss.str();
		}

	private:
		bool m_Iconified;
	};

	/* 窗口内容缩放变化事件：窗口在不同 DPI 显示器之间移动时由 GLFW 回调上报
	 * （glfwSetWindowContentScaleCallback）。Windows 上 content scale = 系统缩放
	 * （150% → 1.5）、且窗口即物理像素；macOS 上恒为 2（点/像素比）。
	 * 消费方（ImGuiLayer）据此重建字体图集并重算 UI 缩放。 */
	class ContentScaleChangedEvent : public AppEvent
	{
	public:
		EVENT_CLASS_TYPE(ContentScaleChanged)

			ContentScaleChangedEvent(float scale_x, float scale_y)
			: m_ScaleX(scale_x), m_ScaleY(scale_y)
		{
		}

		float GetScaleX() const { return m_ScaleX; }
		float GetScaleY() const { return m_ScaleY; }

		std::string ToString() const override
		{
			std::stringstream ss;
			ss << "ContentScaleChangedEvent: " << m_ScaleX << ", " << m_ScaleY;
			return ss.str();
		}

	private:
		float m_ScaleX, m_ScaleY;
	};

	/* 文件拖入事件（GLFW drop 回调）：从系统文件管理器往窗口里拖文件。
	 * Paths 为 UTF-8 形态的绝对路径；X / Y 是拖放落点（窗口逻辑点坐标，
	 * 与 ImGui 的鼠标坐标同一空间）—— 面板据此判断这次拖放落在哪个窗口上。 */
	class WindowFileDropEvent : public AppEvent
	{
	public:
		EVENT_CLASS_TYPE(WindowFileDrop)

			WindowFileDropEvent(std::vector<std::string> paths, float x, float y)
			: m_Paths(std::move(paths)), m_X(x), m_Y(y)
		{
		}

		const std::vector<std::string>& GetPaths() const { return m_Paths; }
		float GetX() const { return m_X; }
		float GetY() const { return m_Y; }

		std::string ToString() const override
		{
			std::stringstream ss;
			ss << "WindowFileDropEvent: " << m_Paths.size() << " file(s) at (" << m_X << ", " << m_Y << ")";
			return ss.str();
		}

	private:
		std::vector<std::string> m_Paths;
		float m_X, m_Y;
	};

	/*
	* 应用Tick事件类
	*/
	class AppTickEvent : public AppEvent
	{
	public:
		EVENT_CLASS_TYPE(AppTick)

			AppTickEvent() = default;
	};

	/*
	* 应用更新事件类
	*/
	class AppUpdateEvent : public AppEvent
	{
	public:
		EVENT_CLASS_TYPE(AppUpdate)

			AppUpdateEvent() = default;
	};

	/*
	* 应用渲染事件类
	*/
	class AppRenderEvent : public AppEvent
	{
	public:
		EVENT_CLASS_TYPE(AppRender)

			AppRenderEvent() = default;
	};
}