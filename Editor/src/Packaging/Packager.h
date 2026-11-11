#pragma once

#include <filesystem>
#include <string>

namespace Helios::Build
{
	/* 打包目标：三种都是「对当前仓库跑一遍 CMake」，区别只在 sysroot、配置与生成器 */
	enum class PackageTarget
	{
		/* 模拟器可直装的应用包 */
		IOSSimulatorApp,
		/* 真机应用包，签名要事先配好 */
		IOSDeviceApp,
		/* Xcode 工程：签名与运行交给 Xcode 自己管 */
		IOSXcodeProject,
	};

	struct PackageResult
	{
		bool Success{ false };
		/* 产物路径：.app 或 .xcodeproj；失败时为空 */
		std::string OutputPath;
		/* 输出的最后若干行，失败时看这个 */
		std::string LogTail;
	};

	/* 跑一次打包，阻塞到结束。界面不要直接调，走 StartAsync。
	 * project_dir 是打进包里的工程（当前打开的那个）；空则用 CMake 配的默认工程。 */
	PackageResult Run(PackageTarget target, const std::filesystem::path& project_dir = {});

	/* 后台打包：同一时间只允许一个任务，正在跑时再调会被忽略 */
	void StartAsync(PackageTarget target, const std::filesystem::path& project_dir = {});
	[[nodiscard]] bool IsRunning();
	/* 正在打包的目标名，给界面显示 */
	[[nodiscard]] const char* RunningLabel();
	/* 取回已完成的结果；还没有新结果返回 false */
	bool Poll(PackageResult& out);

	/* 在系统文件管理器里定位到产物 */
	void RevealInFileManager(const std::string& path);
}
