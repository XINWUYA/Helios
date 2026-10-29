#pragma once
#include <imgui.h>

namespace Helios
{
	/* 停靠面板的窗口外壳：Begin / End 成对由它管，面板只判断要不要画内容。
	 * visible 为 false 时整个窗口不提交（恢复入口在 View 菜单）；被别的页签挡住或折叠时
	 * 照常提交、内容跳过。SetNextWindow* 只能在确定会提交时调，关着的窗口要先返回。 */
	class PanelWindow
	{
	public:
		explicit PanelWindow(const char* name, bool* visible = nullptr, ImGuiWindowFlags flags = 0)
			: m_Submitted(visible == nullptr || *visible)
		{
			if (m_Submitted)
				m_Drawable = ImGui::Begin(name, visible, flags);
		}

		~PanelWindow()
		{
			if (m_Submitted)
				ImGui::End();
		}

		PanelWindow(const PanelWindow&) = delete;
		PanelWindow& operator=(const PanelWindow&) = delete;

		/* 本帧要不要画内容 */
		explicit operator bool() const { return m_Drawable; }

	private:
		bool m_Submitted{ false };
		bool m_Drawable{ false };
	};
}
