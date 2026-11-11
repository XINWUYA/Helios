#include "Pch.h"
#include "AssetPath.h"
#include "Helios/Core/Logger.h"
#include <cstdlib>
#include <string_view>

namespace Helios
{
	std::filesystem::path g_ProjectPath = PathFromUtf8(HELIOS_PROJECT_PATH);
	std::filesystem::path g_EngineResPath = PathFromUtf8(ENGINE_RES_PATH);

	namespace
	{
		uint32_t s_ProjectPathGeneration = 1;

		/* 相对路径里出现 .. 就说明跑到根外面去了，这一份不能当资产相对路径用 */
		bool IsInsideRoot(const std::filesystem::path& relative)
		{
			for (const std::filesystem::path& part : relative)
			{
				if (part == "..")
					return false;
			}
			return !relative.empty();
		}

		/* 这个目录是不是一个能用的资源根：底下得有 EngineRes */
		bool HasEngineRes(const std::filesystem::path& root)
		{
			if (root.empty())
				return false;

			std::error_code error;
			return std::filesystem::is_directory(root / "EngineRes", error);
		}

		/* 可执行文件旁的 Project 目录：打包时随包的单个工程，带 Project.xml 才认。
		 * 认到就换过去 —— 包里那份才是这个包该跑的资源。 */
		bool UseBundledProject(const std::filesystem::path& dir)
		{
			if (dir.empty())
				return false;

			const std::filesystem::path bundled = dir / "Project";
			std::error_code error;
			if (!std::filesystem::is_regular_file(bundled / "Project.xml", error))
				return false;

			g_ProjectPath = bundled;
			++s_ProjectPathGeneration;
			CORE_LOG_INFO("Project root (bundled): {}", PathToUtf8(g_ProjectPath));
			return true;
		}
	}

	bool LocateAssetRoots(const std::filesystem::path& executable_path)
	{
		std::filesystem::path executable = executable_path;
		if (executable.empty())
			executable = GetExecutablePath();

		if (!executable.empty())
		{
			const std::filesystem::path dir = executable.parent_path();

			/* 平铺打包（Windows / Linux / 可执行文件与 EngineRes 同级）、
			 * 包内 Resources（macOS 与 iOS 的 .app）、包根 */
			const std::filesystem::path candidates[] = {
				dir,
				dir.parent_path() / "Resources",
				dir.parent_path(),
			};

			for (const std::filesystem::path& candidate : candidates)
			{
				if (!HasEngineRes(candidate))
					continue;

				g_EngineResPath = candidate / "EngineRes";
				CORE_LOG_INFO("Engine resource root: {}", PathToUtf8(g_EngineResPath));

				/* 包里带了工程就跟着包走；没带才继续用编译期那份（开发态） */
				UseBundledProject(dir);
				return true;
			}
		}

		/* 产物和源码在同一台机器：直接用源码树里那份 */
		g_EngineResPath = PathFromUtf8(ENGINE_RES_PATH);
		CORE_LOG_INFO("Engine resource root (source tree): {}", PathToUtf8(g_EngineResPath));

		if (!executable.empty())
			UseBundledProject(executable.parent_path());

		return false;
	}

	std::filesystem::path GetEngineProjectsDirectory()
	{
		return PathFromUtf8(HELIOS_PROJECT_PATH).parent_path();
	}

	std::filesystem::path GetWritableCacheDirectory()
	{
#if HELIOS_PLATFORM_IOS
		/* 沙箱里唯一能随便写的位置；系统会按空间压力清理，缓存本来就是可丢的 */
		if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
			return PathFromUtf8(home) / "Library" / "Caches";

		/* HOME 取不到（理论上不会）就退临时目录，总比不落盘强 */
		std::error_code error;
		const std::filesystem::path temp = std::filesystem::temp_directory_path(error);
		return error ? std::filesystem::path{} : temp;
#else
		/* 桌面跟项目走；Android 正式接入时换成平台给的私有目录，暂缓项 */
		return g_ProjectPath / "Cache";
#endif
	}

	std::filesystem::path GetWritableLogDirectory()
	{
#if HELIOS_PLATFORM_APPLE
		/* 双击 .app 启动时 cwd 是 "/"，工程目录又受隐私保护，只有这里一定写得进去 */
		if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
			return PathFromUtf8(home) / "Library" / "Logs" / "Helios";
#endif
		return {};
	}

	std::filesystem::path ResolveAssetPath(const std::filesystem::path& path)
	{
		if (path.empty() || path.is_absolute())
			return path;

		std::error_code error;
		const std::filesystem::path project_path = g_ProjectPath / path;
		if (std::filesystem::exists(project_path, error))
			return project_path;

		const std::filesystem::path engine_path = g_EngineResPath / path;
		if (std::filesystem::exists(engine_path, error))
			return engine_path;

		return project_path;
	}

	std::filesystem::path ResolveProjectPath(const std::filesystem::path& path)
	{
		return path.is_absolute() ? path : (g_ProjectPath / path);
	}

	std::filesystem::path ToAssetRelative(const std::filesystem::path& path)
	{
		if (!path.is_absolute())
			return path;

		for (const std::filesystem::path& root : { g_ProjectPath, g_EngineResPath })
		{
			std::error_code error;
			const std::filesystem::path relative = std::filesystem::relative(path, root, error);
			if (!error && IsInsideRoot(relative))
				return relative;
		}

		return path;
	}

	bool SetProjectPath(const std::filesystem::path& path)
	{
		if (path.empty())
			return false;

		std::error_code error;
		const std::filesystem::path absolute = std::filesystem::absolute(path, error);
		if (error)
			return false;

		if (!std::filesystem::is_directory(absolute, error))
		{
			CORE_LOG_ERROR("Project directory not found: {}", PathToUtf8(absolute));
			return false;
		}

		g_ProjectPath = absolute;
		++s_ProjectPathGeneration;
		CORE_LOG_INFO("Project root: {}", PathToUtf8(g_ProjectPath));
		return true;
	}

	uint32_t GetProjectPathGeneration()
	{
		return s_ProjectPathGeneration;
	}

	bool ParseProjectArgument(int argc, const char* const* argv)
	{
		if (argv == nullptr)
			return false;

		bool specified = false;
		for (int index = 1; index < argc; ++index)
		{
			const std::string_view argument = argv[index] != nullptr ? argv[index] : "";
			std::string_view value;

			if (argument.rfind("--project=", 0) == 0)
				value = argument.substr(10);
			else if (argument == "--project" && index + 1 < argc)
				value = argv[++index] != nullptr ? argv[index] : "";
			else
				continue;

			std::filesystem::path path;
			if (TryPathFromUtf8(value, path) && SetProjectPath(path))
				specified = true;
		}

		return specified;
	}
}
