#include "Pch.h"
#include "ScriptModule.h"
#include "Helios/Common/PathUtils.h"
#include "Helios/Script/ScriptRegistry.h"

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace Helios
{
	namespace
	{
		/* 同一路径第二次 dlopen 会命中 dyld 的缓存，库的静态初始化不会再跑一遍 ——
		 * 表清了却没有新类注册，下拉就空了。复制成新文件名再加载，才是一份新映像。 */
		std::string MakeUniqueCopy(const std::string& source)
		{
			namespace fs = std::filesystem;

			static int s_Sequence = 0;

			std::error_code error;
			const fs::path source_path = PathFromUtf8(source);
			const fs::path directory = source_path.parent_path() / "Loaded";
			fs::create_directories(directory, error);
			if (error)
				return source;

			/* 旧库还映射在进程里：macOS 删文件不影响已映射的映像，
			 * Windows 上删不掉的留着，都不妨碍加载新副本 */
			for (const fs::directory_entry& entry : fs::directory_iterator(directory, error))
				fs::remove(entry.path(), error);

			const fs::path target = directory /
				(PathToUtf8(source_path.stem()) + "_" + std::to_string(++s_Sequence) + ".hsmod");
			fs::copy_file(source_path, target, fs::copy_options::overwrite_existing, error);
			return error ? source : PathToUtf8(target);
		}
	}

	ScriptModule::~ScriptModule()
	{
		/* 故意不卸载：脚本库里的静态对象、TLS 与回调指针都可能被别处引用，
		 * 卸载后表现在清了，但旧库留在进程里最省心（一次几十 KB）。 */
	}

	std::string ScriptModule::FindLatestModule(const std::string& output_directory)
	{
		namespace fs = std::filesystem;

		std::error_code error;
		const fs::path directory = PathFromUtf8(output_directory);
		if (!fs::exists(directory, error))
			return {};

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

	bool ScriptModule::LoadLatest()
	{
		return LoadFile(FindLatestModule());
	}

	bool ScriptModule::Recompile()
	{
		if (!Compile())
		{
			m_Status = "Compile failed";
			return false;
		}
		return LoadFile(FindLatestModule());
	}

	int ScriptModule::GetClassCount() const
	{
		return static_cast<int>(ScriptRegistry::Instance().All().size());
	}

	bool ScriptModule::Compile()
	{
		/* 走 CMake target 而不是手拼编译器命令行：编译参数与引擎天然一致，
		 * 少一整套"宏传丢了就随机崩"的坑。 */
		std::string command = std::string("cmake --build \"") + HELIOS_BUILD_DIR + "\" --target HeliosScripts 2>&1";
#ifdef PLATFORM_WINDOWS
		/* 多配置生成器（Visual Studio）要指定配置；单配置生成器忽略这个参数也无害 */
		command = std::string("cmake --build \"") + HELIOS_BUILD_DIR + "\" --target HeliosScripts --config Debug 2>&1";
#endif

		CORE_LOG_INFO("[Script] {}", command);

#ifdef PLATFORM_WINDOWS
		FILE* pipe = _popen(command.c_str(), "r");
#else
		FILE* pipe = popen(command.c_str(), "r");
#endif
		if (pipe == nullptr)
		{
			CORE_LOG_ERROR("[Script] Failed to start the build process.");
			return false;
		}

		char buffer[1024];
		while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr)
		{
			std::string line(buffer);
			while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
				line.pop_back();
			if (line.empty())
				continue;

			/* 编译错误直接进日志面板，别让人去翻终端 */
			if (line.find("error:") != std::string::npos)
				CORE_LOG_ERROR("[Script] {}", line);
			else
				CORE_LOG_INFO("[Script] {}", line);
		}

#ifdef PLATFORM_WINDOWS
		const int exit_code = _pclose(pipe);
#else
		const int exit_code = pclose(pipe);
#endif
		return exit_code == 0;
	}

	bool ScriptModule::LoadFile(const std::string& path)
	{
		if (path.empty())
		{
			m_Status = "No script module built";
			CORE_LOG_WARN("[Script] No .hsmod found under {}.", HELIOS_SCRIPT_OUTPUT_DIR);
			return false;
		}

		/* 先备份再清表：加载失败把旧表放回去，编辑器不至于突然一个类都没有 */
		const std::vector<ScriptClassDesc> backup = ScriptRegistry::Instance().All();
		ScriptRegistry::Instance().Clear();

		const std::string load_path = MakeUniqueCopy(path);

#ifdef PLATFORM_WINDOWS
		HMODULE handle = ::LoadLibraryA(load_path.c_str());
		if (handle == nullptr)
		{
			ScriptRegistry::Instance().Restore(backup);
			m_Status = "Load failed";
			CORE_LOG_ERROR("[Script] LoadLibrary failed: {}", load_path);
			return false;
		}
#else
		void* handle = dlopen(load_path.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (handle == nullptr)
		{
			const char* error = dlerror();
			CORE_LOG_ERROR("[Script] dlopen failed: {}", error != nullptr ? error : "unknown error");
			ScriptRegistry::Instance().Restore(backup);
			m_Status = "Load failed";
			return false;
		}
#endif

		/* 类的注册是库的静态初始化做的，dlopen 返回时已经登记完 */
		m_hModule = handle;
		m_Status = "Loaded " + std::to_string(GetClassCount()) + " script class(es)";
		CORE_LOG_INFO("[Script] {}", m_Status);
		return true;
	}
}
