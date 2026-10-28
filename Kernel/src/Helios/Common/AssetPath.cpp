#include "Pch.h"
#include "AssetPath.h"
#include "Helios/Core/Logger.h"
#include <string_view>

namespace Helios
{
	std::filesystem::path g_ProjectPath = PathFromUtf8(HELIOS_PROJECT_PATH);

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
