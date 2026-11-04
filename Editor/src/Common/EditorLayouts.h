#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace Helios::EditorLayouts
{
	/* 一套自定义布局 = 一份 imgui 布局快照（<项目>/.helios/layouts/<名字>.ini）。
	 * 快照里有每个窗口的位置、停靠节点和选中页签，切布局就是整段灌回 ImGui。 */
	struct Entry
	{
		std::string Name;
		std::filesystem::path Path;
	};

	/* 已保存的布局，按名字排序 */
	std::vector<Entry> List();

	/* 把当前布局存成这个名字（覆盖同名）。名字非法或写盘失败返回 false */
	bool Save(const std::string& name);

	/* 应用一套布局：下一帧按新节点重排 */
	bool Load(const Entry& entry);
}
