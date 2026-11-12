#include "Pch.h"
#include "ProjectScripts.h"

#include "Helios/Common/Platform.h"
#include "Helios/Script/ScriptRegistry.h"

#if !HELIOS_PLATFORM_IOS
#include <filesystem>

#include "Helios/Common/AssetPath.h"
#include "Helios/Common/PathUtils.h"
#ifdef PLATFORM_WINDOWS
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

namespace Helios
{
#if !HELIOS_PLATFORM_IOS
	namespace
	{
		/* 目录下最新的 .hsmod：多配置生成器（Xcode / Visual Studio）会再套一层配置目录，
		 * 所以递归找；Loaded 是编辑器热重载留下的运行期副本，不算产物。 */
		std::string FindLatestModule(const std::filesystem::path& directory)
		{
			namespace fs = std::filesystem;

			std::error_code error;
			std::string newest;
			fs::file_time_type newest_time{};
			for (const fs::directory_entry& entry : fs::recursive_directory_iterator(directory, error))
			{
				if (!entry.is_regular_file(error) || entry.path().extension() != ".hsmod")
					continue;
				if (entry.path().parent_path().filename() == "Loaded")
					continue;

				const auto write_time = entry.last_write_time(error);
				if (newest.empty() || write_time > newest_time)
				{
					newest = PathToUtf8(entry.path());
					newest_time = write_time;
				}
			}
			return newest;
		}
	}
#endif

	ProjectScripts::~ProjectScripts()
	{
		/* 故意不卸载：脚本类里的对象与字段回调都还指着这份映像 */
	}

	bool ProjectScripts::LoadLatest()
	{
#if HELIOS_PLATFORM_IOS
		/* iOS 不允许加载没签名的代码：脚本在构建期就编进了主二进制，
		 * 注册对象在进程启动时已经跑过，这里只看表里有没有类 */
		const int count = GetClassCount();
		if (count == 0)
		{
			m_Status = "No scripts linked into the app";
			CORE_LOG_WARN("[Scripts] No script classes are linked into the app.");
			return false;
		}

		m_Status = "Linked " + std::to_string(count) + " script class(es)";
		CORE_LOG_INFO("[Scripts] {}.", m_Status);
		return true;
#else
		const std::filesystem::path directory = g_ProjectPath / "Cache" / "ScriptsBuild";
		const std::string path = FindLatestModule(directory);
		if (path.empty())
		{
			m_Status = "No built module. Open the project in the Editor once";
			CORE_LOG_WARN("[Scripts] No .hsmod under {}.", PathToUtf8(directory));
			return false;
		}

#ifdef PLATFORM_WINDOWS
		m_hModule = ::LoadLibraryA(path.c_str());
		if (m_hModule == nullptr)
		{
			m_Status = "Load failed";
			CORE_LOG_ERROR("[Scripts] LoadLibrary failed: {}", path);
			return false;
		}
#else
		m_hModule = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (m_hModule == nullptr)
		{
			const char* error = dlerror();
			m_Status = "Load failed";
			CORE_LOG_ERROR("[Scripts] dlopen failed: {}", error != nullptr ? error : "unknown error");
			return false;
		}
#endif

		m_Status = "Loaded " + std::to_string(GetClassCount()) + " script class(es)";
		CORE_LOG_INFO("[Scripts] {} from {}.", m_Status, path);
		return true;
#endif
	}

	int ProjectScripts::GetClassCount() const
	{
		return static_cast<int>(ScriptRegistry::Instance().All().size());
	}
}
