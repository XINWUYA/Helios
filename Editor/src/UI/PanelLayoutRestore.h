#pragma once
#include <string>
#include <vector>

namespace Helios
{
	/* 恢复上次关闭时的停靠前台页签。imgui.ini 本来就完整落盘了每个节点的选中页签
	 * （关编辑器时 DestroyContext 无条件保存），只是启动首帧"窗口出现即聚焦"的级联
	 * 会把各区域的选中抢成最后提交的窗口（Log 抢资源浏览器 / Scene 抢 Frame Graph）。
	 * 这里把 ini 记录读回来，等级联过去后逐帧 SetWindowFocus 复原。 */
	class PanelLayoutRestore
	{
	public:
		/* 从 ini 文本挑出待恢复的窗口名：所在节点选中是它自己、且节点里有 ≥2 个窗口
		 * （单窗口节点没有"前后"可恢复）。按面板注册表顺序返回，方便断言。 */
		static std::vector<std::string> ParsePendingTabs(const std::string& ini_text);

		/* 每帧调用一次，须在任何窗口提交之前。 */
		void Update();

	private:
		std::vector<std::string> m_Pending;
		int m_FramesWaited{ 0 };
		bool m_Parsed{ false };
	};
}
