#pragma once

#include <filesystem>

namespace Helios::EditorSettings
{
	/* 编辑器的全局开关：对所有视口生效，跟项目走。
	 * 存 <项目>/.helios/editor.cfg —— 同一份项目换台机器重新开，
	 * 这些调试与画质开关不会被重置回默认。 */
	struct State
	{
		bool MSAA{ true };
		bool GPUSkinning{ true };
		/* 脚本：产物变了自动重载 / 打开项目时没产物自动编一次 */
		bool AutoReloadScripts{ true };
		bool AutoCompileScripts{ true };
		/* 着色器：源文件（含 include 的公共库）改了自动重载 */
		bool AutoReloadShaders{ true };
		/* 编辑器强调色主题（EditorTheme::kThemePresets 的下标） */
		int Theme{ 0 };
	};

	/* 读当前项目的设置；文件缺失时按引擎当前默认值起步 */
	State Load();
	void Save(const State& state);

	/* 把设置推给引擎（打开项目、切换开关都走这里，保证文件与运行态一致） */
	void Apply(const State& state);
}
