#include "Pch.h"
#include "ScriptModule.h"
#include "Helios/Common/AssetPath.h"
#include "Helios/Common/PathUtils.h"
#include "Helios/Script/ScriptRegistry.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

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
		/* 跑一条命令并收下输出：成功只留一行提示，失败把全文打出来 */
		bool RunCommand(const std::string& command)
		{
			/* CMake 的错误走 stderr，不合并的话日志里只剩一句 incomplete */
			const std::string full_command = command + " 2>&1";
			CORE_LOG_INFO("[Script] {}", command);

#ifdef PLATFORM_WINDOWS
			FILE* pipe = _popen(full_command.c_str(), "r");
#else
			FILE* pipe = popen(full_command.c_str(), "r");
#endif
			if (pipe == nullptr)
			{
				CORE_LOG_ERROR("[Script] Failed to start: {}", command);
				return false;
			}

			std::string output;
			char buffer[1024];
			while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr)
				output += buffer;

#ifdef PLATFORM_WINDOWS
			const int exit_code = _pclose(pipe);
#else
			const int exit_code = pclose(pipe);
#endif
			if (exit_code == 0)
				return true;

			CORE_LOG_ERROR("[Script] Command failed: {}", command);
			std::istringstream lines(output);
			for (std::string line; std::getline(lines, line); )
			{
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (!line.empty())
					CORE_LOG_ERROR("[Script] {}", line);
			}
			return false;
		}

		/* Kernel 库所在目录：单配置生成器在 lib/<输出目录>，多配置生成器（Xcode）
		 * 还会再套一层配置名 —— 按实际存在的那份找，别指望配置期猜准。 */
		std::filesystem::path FindKernelLibraryDirectory()
		{
#ifdef PLATFORM_WINDOWS
			constexpr const char* kLibraryName = "Kernel.lib";
#elif defined(PLATFORM_MACOS)
			constexpr const char* kLibraryName = "libKernel.dylib";
#else
			constexpr const char* kLibraryName = "libKernel.so";
#endif
			const std::filesystem::path root =
				PathFromUtf8(HELIOS_BUILD_DIR) / "lib" / HELIOS_BUILD_OUTPUT_DIR;

			std::error_code error;
			if (std::filesystem::exists(root / kLibraryName, error))
				return root;

			const std::filesystem::path configured = root / HELIOS_BUILD_CONFIG;
			if (std::filesystem::exists(configured / kLibraryName, error))
				return configured;

			return root;
		}

		/* 项目 Scripts 下的源文件（递归，按路径排序保证构建稳定） */
		std::vector<std::filesystem::path> CollectScriptSources(const std::filesystem::path& scripts_dir)
		{
			std::vector<std::filesystem::path> sources;

			std::error_code error;
			for (std::filesystem::recursive_directory_iterator iter(scripts_dir,
					std::filesystem::directory_options::skip_permission_denied, error), end;
				iter != end; iter.increment(error))
			{
				if (error)
					break;
				if (!iter->is_regular_file())
					continue;
				if (iter->path().extension() != ".cpp")
					continue;
				sources.push_back(iter->path());
			}

			std::sort(sources.begin(), sources.end());
			return sources;
		}
	}

	ScriptModule::~ScriptModule()
	{
		/* 故意不卸载：脚本库里的静态对象、TLS 与回调指针都可能被别处引用，
		 * 卸载后表现在清了，但旧库留在进程里最省心（一次几十 KB）。 */
	}

	std::filesystem::path ScriptModule::ModuleDirectory()
	{
		return g_ProjectPath / "Cache" / "ScriptsBuild";
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
			/* Loaded 是运行期副本，CMakeFiles 是构建中间件 —— 都不是产物 */
			const std::string parent = entry.path().parent_path().filename().string();
			if (parent == "Loaded" || parent == "CMakeFiles")
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
		const std::string output_directory = PathToUtf8(ModuleDirectory());
		std::string latest = FindLatestModule(output_directory);

		/* 新项目第一次打开：还没有产物，现编一次再加载（自动编译关掉就只报状态） */
		if (latest.empty())
		{
			if (!m_AutoCompileEnabled)
			{
				m_Status = "No module built";
				CORE_LOG_WARN("[Script] No module under {}, auto compile is off.", output_directory);
				return false;
			}

			CORE_LOG_INFO("[Script] No module under {}, building project scripts.", output_directory);
			if (!Compile())
				return false;
			latest = FindLatestModule(output_directory);
		}

		return LoadFile(latest);
	}

	bool ScriptModule::Recompile()
	{
		if (!Compile())
			return false;
		return LoadFile(FindLatestModule(PathToUtf8(ModuleDirectory())));
	}

	int ScriptModule::GetClassCount() const
	{
		return static_cast<int>(ScriptRegistry::Instance().All().size());
	}

	/* 项目级构建：把当前项目的源文件列表写成一个小工程，编译条件由引擎构建期生成的
	 * 模板提供（HELIOS_SCRIPT_TEMPLATE）。产物和中间文件都留在项目 Cache 下，
	 * 项目放哪块盘都能编，引擎构建目录不必知道它在哪。 */
	bool ScriptModule::Compile()
	{
		const std::filesystem::path scripts_dir = g_ProjectPath / "Scripts";
		const std::filesystem::path build_root = ModuleDirectory();
		const std::filesystem::path build_dir = build_root / "build";

		const std::vector<std::filesystem::path> sources = CollectScriptSources(scripts_dir);
		if (sources.empty())
		{
			m_Status = "No scripts";
			CORE_LOG_INFO("[Script] No scripts under {}.", PathToUtf8(scripts_dir));
			return false;
		}

		const std::filesystem::path template_path = PathFromUtf8(HELIOS_SCRIPT_TEMPLATE);
		if (!std::filesystem::is_regular_file(template_path))
		{
			m_Status = "Build template missing";
			CORE_LOG_ERROR("[Script] Script build template not found: {}", PathToUtf8(template_path));
			return false;
		}

		std::error_code error;
		std::filesystem::create_directories(build_root, error);

		{
			std::ofstream project_file(build_root / "CMakeLists.txt", std::ios::trunc);
			if (!project_file)
			{
				m_Status = "Cannot write CMakeLists";
				CORE_LOG_ERROR("[Script] Cannot write {}", PathToUtf8(build_root / "CMakeLists.txt"));
				return false;
			}

			project_file << "# 由编辑器生成，改动会在下次编译时被覆盖\n";
			project_file << "cmake_minimum_required(VERSION 3.20)\n";
			project_file << "project(HeliosProjectScripts LANGUAGES CXX)\n";
			project_file << "set(HELIOS_SCRIPT_SOURCES\n";
			for (const std::filesystem::path& source : sources)
				project_file << "    \"" << PathToUtf8(source) << "\"\n";
			project_file << ")\n";
			project_file << "include(\"" << PathToUtf8(template_path) << "\")\n";
		}

		CORE_LOG_INFO("[Script] Building {} script file(s) of {}.",
			sources.size(), PathToUtf8(g_ProjectPath));

		const std::string kernel_dir = PathToUtf8(FindKernelLibraryDirectory());
		const std::string configure = "cmake -S \"" + PathToUtf8(build_root) + "\" -B \"" + PathToUtf8(build_dir)
			+ "\" -DHELIOS_SCRIPT_CONFIG=" + HELIOS_BUILD_CONFIG
			+ " -DCMAKE_BUILD_TYPE=" + HELIOS_BUILD_CONFIG
			+ " -DHELIOS_SCRIPT_KERNEL_DIR=\"" + kernel_dir + "\"";
		if (!RunCommand(configure))
		{
			m_Status = "Configure failed";
			return false;
		}

		const std::string build = "cmake --build \"" + PathToUtf8(build_dir)
			+ "\" --config " + HELIOS_BUILD_CONFIG;
		if (!RunCommand(build))
		{
			m_Status = "Compile failed";
			return false;
		}

		m_Status = "Compiled";
		return true;
	}

	void ScriptModule::TickAutoReload(float delta_time)
	{
		if (!m_AutoReloadEnabled)
			return;

		constexpr float kPollInterval = 1.0f;
		m_AutoReloadTimer += delta_time;
		if (m_AutoReloadTimer < kPollInterval)
			return;
		m_AutoReloadTimer = 0.0f;

		const std::string latest = FindLatestModule(PathToUtf8(ModuleDirectory()));
		if (latest.empty())
			return;

		std::error_code error;
		const auto write_time = std::filesystem::last_write_time(PathFromUtf8(latest), error);
		if (error)
			return;

		if (!m_AutoReloadPrimed)
		{
			m_LastModuleTime = write_time;
			m_AutoReloadPrimed = true;
			return;
		}

		if (write_time <= m_LastModuleTime)
			return;

		/* 先记时间再加载：加载失败也不该每秒重试刷屏 */
		m_LastModuleTime = write_time;
		CORE_LOG_INFO("[Script] Detected a new module, reloading: {}", latest);
		if (!LoadFile(latest))
			CORE_LOG_ERROR("[Script] Auto-reload failed: {}", latest);
	}

	bool ScriptModule::LoadFile(const std::string& path)
	{
		if (path.empty())
		{
			m_Status = "No script module built";
			CORE_LOG_WARN("[Script] No .hsmod found under {}.", PathToUtf8(ModuleDirectory()));
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

		/* 记下这份产物的时间戳：自动重载的基线就是"最后加载过的那份" */
		std::error_code time_error;
		const auto write_time = std::filesystem::last_write_time(PathFromUtf8(path), time_error);
		if (!time_error)
		{
			m_LastModuleTime = write_time;
			m_AutoReloadPrimed = true;
		}
		return true;
	}
}
