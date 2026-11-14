#pragma once

#include "HeliosConfig.h"
#include "Helios/Common/Export.h"
#include "Helios/Common/PathUtils.h"
#include <filesystem>

namespace Helios
{
	/* 引擎内置资源根：启动时由 LocateAssetRoots 定位，所有项目共享，只读。
	 * 没调定位的场合（headless 检查、工具）它就是编译期路径 —— 源码树里的开发态。 */
	extern HELIOS_API std::filesystem::path g_EngineResPath;

	/* 运行期定位资源根，摆脱编译期绝对路径：按「可执行文件旁 → 包内 Resources →
	 * 编译期路径」依次试，用第一个含 EngineRes 的目录。executable_path 留空时取当前进程的。
	 * 返回是否离开了编译期路径。 */
	HELIOS_API bool LocateAssetRoots(const std::filesystem::path& executable_path = {});

	/* 引擎自带的 Projects/ 目录：启动页列已有工程、新建工程默认落这里。
	 * 取编译期默认工程根的上一级 —— 打包后资源根在包里，工程目录还在源码树，两者不能混。 */
	HELIOS_API std::filesystem::path GetEngineProjectsDirectory();

	/* 项目根：启动时确定（编译期默认 + --project 覆盖），工程里唯一可写根 */
	extern HELIOS_API std::filesystem::path g_ProjectPath;

	/* ---- 可写目录 ----
	 * 只放能重建的东西：着色器产物、管线缓存、日志。打包后的 .app 是只读的，
	 * 桌面跟着项目走，iOS 只能写沙箱，所以两边必须分开取，不能拼项目根。 */

	/* 缓存根。桌面是 <项目>/Cache（改完能直接看到产物）；iOS 是沙箱的 Library/Caches，
	 * 系统可随时清理，丢了重编即可。 */
	HELIOS_API std::filesystem::path GetWritableCacheDirectory();

	/* 日志目录。桌面与 iOS 都是用户目录下的 logs；取不到返回空，调用方退 cwd。 */
	HELIOS_API std::filesystem::path GetWritableLogDirectory();

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

	/* 项目根每换一次加一：按相对路径缓存的句柄据此判断要不要重新解析 */
	HELIOS_API uint32_t GetProjectPathGeneration();

	/* 命令行解析：--project <dir> 或 --project=<dir>，后出现的覆盖前面的。
	 * 返回值表示命令行是否指定了项目（编辑器据此决定直接进工程还是先出启动页）。 */
	HELIOS_API bool ParseProjectArgument(int argc, const char* const* argv);

	/* 命令行解析：--scene <file> 或 --scene=<file>，返回空表示没指定。
	 * 相对路径先按项目内资产找，没有再按工作目录算；要在 ParseProjectArgument 之后调。
	 * 解析结果留在 GetSceneArgument，游戏层按它覆盖项目里配的启动场景。 */
	HELIOS_API std::filesystem::path ParseSceneArgument(int argc, const char* const* argv);

	/* 命令行指定的场景文件，空表示没指定 */
	HELIOS_API const std::filesystem::path& GetSceneArgument();
}
