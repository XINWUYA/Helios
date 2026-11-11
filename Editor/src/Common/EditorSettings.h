#pragma once

#include <string>

namespace Helios::EditorSettings
{
	/* 本机的编辑器状态：主题、热重载、Game UI 面板的视图。
	 * 存 <项目>/.helios/editor.cfg —— 这个目录不入库，换台机器按默认起。
	 * 跟项目走的画质开关在 Project.xml（见 Helios/Application/ProjectConfig.h）。 */
	struct State
	{
		/* 脚本：产物变了自动重载 / 打开项目时没产物自动编一次 */
		bool AutoReloadScripts{ true };
		bool AutoCompileScripts{ true };
		/* 着色器：源文件（含 include 的公共库）改了自动重载 */
		bool AutoReloadShaders{ true };
		/* 编辑器强调色主题（EditorTheme::kThemePresets 的下标） */
		int Theme{ 0 };

		/* 游戏 UI 面板的上次状态：重开编辑器接着看同一份文档、
		 * 同样的分辨率与开关 */
		struct UIPreviewState
		{
			std::string Document{};      /* 相对项目根，空 = 没选过 */
			int Resolution{ 0 };
			bool Guides{ false };
			bool Editing{ false };
			bool Tree{ true };
			float CodeFraction{ 0.40f };
			float TreeFraction{ 0.22f };
			float InspectorFraction{ 0.28f };
		};
		UIPreviewState UIPreview{};
	};

	/* 读当前项目的设置；文件缺失时用默认值 */
	State Load();
	void Save(const State& state);
}
