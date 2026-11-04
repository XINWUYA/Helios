#include "Pch.h"
#include "EditorLayouts.h"
#include "Helios/Common/AssetPath.h"
#include <algorithm>
#include <fstream>
#include <imgui.h>

namespace Helios::EditorLayouts
{
	namespace
	{
		std::filesystem::path LayoutDir()
		{
			return g_ProjectPath / ".helios" / "layouts";
		}

		/* 名字要能直接当文件名：挡掉路径分隔符与 Windows 保留字符 */
		bool IsValidName(const std::string& name)
		{
			if (name.empty() || name.size() > 64)
				return false;

			for (const char c : name)
			{
				if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?'
					|| c == '"' || c == '<' || c == '>' || c == '|')
					return false;
			}
			return true;
		}
	}

	std::vector<Entry> List()
	{
		std::vector<Entry> entries;

		std::error_code error;
		if (!std::filesystem::exists(LayoutDir(), error))
			return entries;

		for (const auto& item : std::filesystem::directory_iterator(LayoutDir(), error))
		{
			if (!item.is_regular_file() || item.path().extension() != ".ini")
				continue;

			entries.push_back({ PathToUtf8(item.path().stem()), item.path() });
		}

		std::sort(entries.begin(), entries.end(),
			[](const Entry& lhs, const Entry& rhs) { return lhs.Name < rhs.Name; });
		return entries;
	}

	bool Save(const std::string& name)
	{
		if (!IsValidName(name))
			return false;

		std::error_code error;
		std::filesystem::create_directories(LayoutDir(), error);

		/* 取当前布局：imgui 把窗口位置、停靠节点、选中页签全写在返回的这段文本里 */
		size_t size = 0;
		const char* text = ImGui::SaveIniSettingsToMemory(&size);
		if (text == nullptr || size == 0)
			return false;

		std::ofstream file(LayoutDir() / (name + ".ini"), std::ios::binary | std::ios::trunc);
		if (!file.is_open())
			return false;

		file.write(text, static_cast<std::streamsize>(size));
		return file.good();
	}

	bool Load(const Entry& entry)
	{
		std::ifstream file(entry.Path, std::ios::binary);
		if (!file.is_open())
			return false;

		const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		if (text.empty())
			return false;

		/* 灌回去即可：DockSpace 节点在下一帧按这份数据重建，不会再走默认布局 */
		ImGui::LoadIniSettingsFromMemory(text.c_str(), text.size());
		return true;
	}
}
