#include "Pch.h"
#include "EditorSettings.h"
#include "Helios/ImGui/EditorTheme.h"
#include "Helios/Renderer/Renderer.h"
#include "Helios/Scene/SkeletonModel.h"
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
		/* 缺哪个键就保留引擎当前默认，只覆盖文件里写过的 */
		State state;
		state.MSAA = Renderer::IsMSAAEnabled();
		state.GPUSkinning = SkeletonModel::IsGlobalGpuSkinningEnabled();

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
			if (key == "MSAA")
				state.MSAA = value;
			else if (key == "GPUSkinning")
				state.GPUSkinning = value;
			else if (key == "AutoReloadScripts")
				state.AutoReloadScripts = value;
			else if (key == "AutoCompileScripts")
				state.AutoCompileScripts = value;
			else if (key == "AutoReloadShaders")
				state.AutoReloadShaders = value;
			else if (key == "Theme")
				state.Theme = std::atoi(raw.c_str());
		}

		/* 配置文件里的主题序号可能来自旧版本（主题表增删过），越界就回落到默认档 */
		if (state.Theme < 0 || state.Theme >= EditorTheme::ThemeCount())
			state.Theme = 0;

		EDITOR_LOG_INFO("Editor settings: MSAA {}, GPU skinning {}, theme {}",
			state.MSAA ? "on" : "off", state.GPUSkinning ? "on" : "off", state.Theme);
		return state;
	}

	void Save(const State& state)
	{
		std::error_code error;
		std::filesystem::create_directories(SettingsPath().parent_path(), error);

		std::ofstream file(SettingsPath(), std::ios::trunc);
		if (!file)
			return;

		file << "MSAA=" << (state.MSAA ? 1 : 0) << "\n";
		file << "GPUSkinning=" << (state.GPUSkinning ? 1 : 0) << "\n";
		file << "AutoReloadScripts=" << (state.AutoReloadScripts ? 1 : 0) << "\n";
		file << "AutoCompileScripts=" << (state.AutoCompileScripts ? 1 : 0) << "\n";
		file << "AutoReloadShaders=" << (state.AutoReloadShaders ? 1 : 0) << "\n";
		file << "Theme=" << state.Theme << "\n";
	}

	void Apply(const State& state)
	{
		Renderer::SetMSAAEnabled(state.MSAA);
		SkeletonModel::SetGlobalGpuSkinningEnabled(state.GPUSkinning);
		EditorTheme::SetTheme(state.Theme);
	}
}
