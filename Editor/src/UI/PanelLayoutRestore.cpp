#include "Pch.h"
#include "PanelLayoutRestore.h"
#include "PanelRegistry.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>

namespace Helios
{
	namespace
	{
		/* 等过首帧的聚焦级联（1 帧就够，留一帧余量给首启建布局） */
		constexpr int kSettleFrames = 2;

		/* "……=0x……" 后面的十六进制值（匹配不到返回 0；窗口 id 非 0，0 即"无"） */
		unsigned ParseHexAfter(const std::string& text, const char* key)
		{
			const size_t pos = text.find(key);
			if (pos == std::string::npos)
				return 0;
			return static_cast<unsigned>(std::strtoul(text.c_str() + pos + std::strlen(key), nullptr, 16));
		}

		/* [Docking] 树里某节点行（DockNode/DockSpace + 该节点 ID）记录的 Selected 页签 */
		unsigned SelectedOfNode(const std::string& ini_text, const char* node_needle)
		{
			for (size_t line_begin = 0; line_begin < ini_text.size();)
			{
				size_t line_end = ini_text.find('\n', line_begin);
				if (line_end == std::string::npos)
					line_end = ini_text.size();
				const std::string line = ini_text.substr(line_begin, line_end - line_begin);

				if ((line.find("DockNode") != std::string::npos || line.find("DockSpace") != std::string::npos)
					&& line.find(node_needle) != std::string::npos)
					return ParseHexAfter(line, " Selected=0x");

				line_begin = line_end + 1;
			}
			return 0;
		}
	}

	std::vector<std::string> PanelLayoutRestore::ParsePendingTabs(const std::string& ini_text)
	{
		/* 先扫 [Window][…] 节：名字 → 节点 id，同时数出每个节点挂几个窗口 */
		std::map<std::string, unsigned> window_node;
		std::map<unsigned, int> node_window_count;
		for (size_t cursor = ini_text.find("[Window]["); cursor != std::string::npos;)
		{
			const size_t name_begin = cursor + std::strlen("[Window][");
			const size_t name_end = ini_text.find(']', name_begin);
			if (name_end == std::string::npos)
				break;

			const std::string name = ini_text.substr(name_begin, name_end - name_begin);
			const size_t body_end = ini_text.find("\n[", name_end);
			const std::string body = ini_text.substr(name_end, body_end - name_end);

			const unsigned node_id = ParseHexAfter(body, "DockId=0x");
			if (node_id != 0)
			{
				window_node[name] = node_id;
				node_window_count[node_id] += 1;
			}

			if (body_end == std::string::npos)
				break;
			cursor = ini_text.find("[Window][", body_end);
		}

		std::vector<std::string> pending;
		for (const auto& panel : Panel::kDefaultLayout)
		{
			const auto node_it = window_node.find(panel.Id);
			if (node_it == window_node.end() || node_window_count[node_it->second] < 2)
				continue;

			char needle[32];
			std::snprintf(needle, sizeof(needle), "ID=0x%08X", node_it->second);
			if (SelectedOfNode(ini_text, needle) == ImHashStr(panel.Id))
				pending.emplace_back(panel.Id);
		}
		return pending;
	}

	void PanelLayoutRestore::Update()
	{
		if (!m_Parsed)
		{
			if (++m_FramesWaited < kSettleFrames)
				return;
			m_Parsed = true;

			/* 与布局持久化同源：路径直接取 io.IniFilename（冻结保存期间为 nullptr，跳过） */
			if (const char* ini_path = ImGui::GetIO().IniFilename)
			{
				std::ifstream file(ini_path);
				const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
				m_Pending = ParsePendingTabs(text);
			}
			return;
		}

		if (m_Pending.empty())
			return;

		/* 一帧恢复一个：SetWindowFocus 会把焦点和所在节点的前台页签一起切过去
		 * （节点刷新的回写规则），同帧多个会互相覆盖。窗口不活跃（已隐藏/未提交）
		 * 就跳过，该节点维持现状。 */
		const std::string name = m_Pending.front();
		m_Pending.erase(m_Pending.begin());

		ImGuiWindow* window = ImGui::FindWindowByName(name.c_str());
		if (window != nullptr && window->LastFrameActive + 1 >= ImGui::GetFrameCount())
			ImGui::SetWindowFocus(name.c_str());
	}
}
