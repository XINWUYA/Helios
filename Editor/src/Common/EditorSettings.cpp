#include "Pch.h"
#include "EditorSettings.h"
#include "EditorUI/EditorTheme.h"
#include <cstdlib>
#include <fstream>

namespace Helios::EditorSettings
{
	namespace
	{
		std::filesystem::path SettingsPath()
		{
			return g_ProjectPath / ".helios" / "editor.cfg";
		}
	}

	State Load()
	{
		/* 缺哪个键就保留默认，只覆盖文件里写过的 */
		State state;

		std::ifstream file(SettingsPath());
		for (std::string line; std::getline(file, line); )
		{
			if (!line.empty() && line.back() == '\r')
				line.pop_back();

			const size_t separator = line.find('=');
			if (separator == std::string::npos)
				continue;

			const std::string key = line.substr(0, separator);
			const std::string raw = line.substr(separator + 1);
			const bool value = raw != "0";
			if (key == "AutoReloadScripts")
				state.AutoReloadScripts = value;
			else if (key == "AutoCompileScripts")
				state.AutoCompileScripts = value;
			else if (key == "AutoReloadShaders")
				state.AutoReloadShaders = value;
			else if (key == "Theme")
				state.Theme = std::atoi(raw.c_str());
			else if (key == "UIPreviewDocument")
				state.UIPreview.Document = raw;
			else if (key == "UIPreviewResolution")
				state.UIPreview.Resolution = std::atoi(raw.c_str());
			else if (key == "UIPreviewGuides")
				state.UIPreview.Guides = value;
			else if (key == "UIPreviewEditing")
				state.UIPreview.Editing = value;
			else if (key == "UIPreviewTree")
				state.UIPreview.Tree = value;
			else if (key == "UIPreviewTreeFraction")
				state.UIPreview.TreeFraction = static_cast<float>(std::atof(raw.c_str()));
			else if (key == "UIPreviewCodeFraction")
				state.UIPreview.CodeFraction = static_cast<float>(std::atof(raw.c_str()));
			else if (key == "UIPreviewInspectorFraction")
				state.UIPreview.InspectorFraction = static_cast<float>(std::atof(raw.c_str()));
		}

		/* 配置文件里的主题序号可能来自旧版本（主题表增删过），越界就回落到默认档 */
		if (state.Theme < 0 || state.Theme >= EditorTheme::ThemeCount())
			state.Theme = 0;

		EDITOR_LOG_INFO("Editor settings: theme {}, auto reload scripts {}",
			state.Theme, state.AutoReloadScripts ? "on" : "off");
		return state;
	}

	void Save(const State& state)
	{
		std::error_code error;
		std::filesystem::create_directories(SettingsPath().parent_path(), error);

		std::ofstream file(SettingsPath(), std::ios::trunc);
		if (!file)
			return;

		file << "AutoReloadScripts=" << (state.AutoReloadScripts ? 1 : 0) << "\n";
		file << "AutoCompileScripts=" << (state.AutoCompileScripts ? 1 : 0) << "\n";
		file << "AutoReloadShaders=" << (state.AutoReloadShaders ? 1 : 0) << "\n";
		file << "Theme=" << state.Theme << "\n";
		file << "UIPreviewDocument=" << state.UIPreview.Document << "\n";
		file << "UIPreviewResolution=" << state.UIPreview.Resolution << "\n";
		file << "UIPreviewGuides=" << (state.UIPreview.Guides ? 1 : 0) << "\n";
		file << "UIPreviewEditing=" << (state.UIPreview.Editing ? 1 : 0) << "\n";
		file << "UIPreviewTree=" << (state.UIPreview.Tree ? 1 : 0) << "\n";
		file << "UIPreviewTreeFraction=" << state.UIPreview.TreeFraction << "\n";
		file << "UIPreviewCodeFraction=" << state.UIPreview.CodeFraction << "\n";
		file << "UIPreviewInspectorFraction=" << state.UIPreview.InspectorFraction << "\n";
	}
}
