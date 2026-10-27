#pragma once

#include "HeliosConfig.h"
#include "Helios/Common/Export.h"
#include "Helios/Common/PathUtils.h"
#include <filesystem>

namespace Helios
{
	/* 引擎内置资源根：编译期固定，随引擎走，所有项目共享，只读 */
	inline const std::filesystem::path g_EngineResPath = PathFromUtf8(ENGINE_RES_PATH);

	/* 项目根：启动时确定（编译期默认 + --project 覆盖），工程里唯一可写根 */
	extern HELIOS_API std::filesystem::path g_ProjectPath;

	/* 读路径：项目根优先，找不到回落引擎根 —— 项目里放同名文件即可覆盖内置资源。
	 * 两个根都没有时返回项目根下的路径，给调用方一个能报出来的位置。 */
	HELIOS_API std::filesystem::path ResolveAssetPath(const std::filesystem::path& path);

	/* 写路径：只拼项目根，绝不落进引擎根 */
	HELIOS_API std::filesystem::path ResolveProjectPath(const std::filesystem::path& path);

	/* 绝对转相对：两个根各试一次，都不在根下就原样返回。
	 * 不能只对项目根做一次 std::filesystem::relative —— 引擎资源的相对路径会变成 ../ 开头。 */
	HELIOS_API std::filesystem::path ToAssetRelative(const std::filesystem::path& path);

	/* 启动期换项目，目录不存在返回 false */
	HELIOS_API bool SetProjectPath(const std::filesystem::path& path);

	/* 命令行解析：--project <dir> 或 --project=<dir>，后出现的覆盖前面的。
	 * 返回值表示命令行是否指定了项目（编辑器据此决定直接进工程还是先出启动页）。 */
	HELIOS_API bool ParseProjectArgument(int argc, const char* const* argv);
}
