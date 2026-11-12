#include "Pch.h"
#include "Packager.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#define HELIOS_POPEN _popen
#define HELIOS_PCLOSE _pclose
#else
#define HELIOS_POPEN popen
#define HELIOS_PCLOSE pclose
#endif

namespace Helios::Build
{
	namespace
	{
		/* 一个任务的状态：界面每帧来问，后台线程只往这里写 */
		struct TaskState
		{
			std::mutex Mutex;
			bool Running{ false };
			const char* Label{ "" };
			const char* Stage{ "" };
			float Fraction{ -1.0f };
			std::string LastLine;
			PackageResult Pending{};
			bool HasPending{ false };
		};

		TaskState s_State;

		std::filesystem::path EngineRoot()
		{
			return std::filesystem::path(HELIOS_ENGINE_ROOT_DIR);
		}

		std::string Quoted(const std::filesystem::path& path)
		{
			return "\"" + path.string() + "\"";
		}

		/* 从 "[ 42%] Building CXX object ..." 里取百分比 */
		bool ParsePercent(const std::string& line, float& fraction)
		{
			if (line.empty() || line[0] != '[')
				return false;

			const size_t percent = line.find('%');
			if (percent == std::string::npos || percent + 1 >= line.size() || line[percent + 1] != ']')
				return false;

			int value = 0;
			bool has_digit = false;
			for (size_t i = 1; i < percent; ++i)
			{
				if (line[i] < '0' || line[i] > '9')
					continue;

				value = value * 10 + (line[i] - '0');
				has_digit = true;
			}

			if (!has_digit)
				return false;

			fraction = static_cast<float>(value) / 100.0f;
			return true;
		}

		const char* LabelOf(PackageTarget target)
		{
			switch (target)
			{
			case PackageTarget::IOSSimulatorApp: return "打包 iOS 模拟器应用";
			case PackageTarget::IOSDeviceApp: return "打包 iOS 真机应用";
			case PackageTarget::IOSXcodeProject: return "生成 Xcode 工程";
			}
			return "打包";
		}

		/* 后台线程写进度：界面每帧来读，读写同一把锁。
		 * fraction 传负数表示这一步没有百分比（CMake 配置阶段不打） */
		void ReportProgress(const char* stage, float fraction)
		{
			std::lock_guard<std::mutex> lock(s_State.Mutex);
			s_State.Stage = stage;
			s_State.Fraction = fraction;
		}

		/* 跑一条命令，输出逐行转发到日志，末尾若干行留给界面。
		 * progress_span > 0 时把输出里的 [NN%] 折算进整体进度；base 传负数是"这一步没有百分比" */
		bool RunCommand(const std::string& command, std::string& log_tail,
			const char* stage, float progress_base, float progress_span)
		{
			ReportProgress(stage, progress_base);

			std::FILE* pipe = HELIOS_POPEN(command.c_str(), "r");
			if (pipe == nullptr)
			{
				log_tail = "无法启动命令：" + command;
				return false;
			}

			std::array<char, 1024> buffer{};
			std::deque<std::string> lines;
			while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
			{
				std::string line = buffer.data();
				while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
					line.pop_back();
				if (line.empty())
					continue;

				CORE_LOG_INFO("[Package] {}", line);

				float percent = 0.0f;
				const bool has_percent = progress_span > 0.0f && ParsePercent(line, percent);
				{
					std::lock_guard<std::mutex> lock(s_State.Mutex);
					s_State.LastLine = line;
					if (has_percent)
					{
						s_State.Stage = stage;
						s_State.Fraction = progress_base + progress_span * percent;
					}
				}

				lines.push_back(std::move(line));
				if (lines.size() > 40)
					lines.pop_front();
			}

			const int status = HELIOS_PCLOSE(pipe);

			log_tail.clear();
			for (const std::string& line : lines)
				log_tail += line + "\n";

			return status == 0;
		}

		/* 现有构建目录的生成器变了就先删掉：CMake 不允许原地换生成器 */
		void ResetBuildDirIfGeneratorDiffers(const std::filesystem::path& build_dir, const char* generator)
		{
			const std::filesystem::path cache = build_dir / "CMakeCache.txt";
			std::error_code error;
			if (!std::filesystem::exists(cache, error))
				return;

			const std::string expected = std::string("CMAKE_GENERATOR:INTERNAL=") + generator;
			std::FILE* file = std::fopen(cache.string().c_str(), "rb");
			if (file == nullptr)
				return;

			std::string content;
			std::array<char, 4096> buffer{};
			while (const size_t read = std::fread(buffer.data(), 1, buffer.size(), file))
				content.append(buffer.data(), read);
			std::fclose(file);

			if (content.find(expected) != std::string::npos)
				return;

			CORE_LOG_INFO("[Package] 生成器变了，重建构建目录：{}", build_dir.string());
			std::filesystem::remove_all(build_dir, error);
		}

		/* 在构建目录里找产物：应用包在 bin 下的配置目录里，Xcode 工程在根上 */
		std::string FindApp(const std::filesystem::path& build_dir)
		{
			std::error_code error;
			const std::filesystem::path bin = build_dir / "bin";
			if (!std::filesystem::is_directory(bin, error))
				return {};

			for (const auto& entry : std::filesystem::recursive_directory_iterator(bin, error))
			{
				if (entry.is_directory(error) && entry.path().extension() == ".app")
					return entry.path().string();
			}
			return {};
		}

		std::string FindXcodeProject(const std::filesystem::path& build_dir)
		{
			std::error_code error;
			if (!std::filesystem::is_directory(build_dir, error))
				return {};

			for (const auto& entry : std::filesystem::directory_iterator(build_dir, error))
			{
				if (entry.is_directory(error) && entry.path().extension() == ".xcodeproj")
					return entry.path().string();
			}
			return {};
		}

		PackageResult RunImpl(PackageTarget target, const std::filesystem::path& project_dir)
		{
			PackageResult result;

			std::string reason;
			if (!CheckEnvironment(reason))
			{
				result.LogTail = reason;
				return result;
			}

			const std::filesystem::path root = EngineRoot();
			const std::string cmake = "\"" + std::string(HELIOS_CMAKE_PATH) + "\"";
			const std::string toolchain = Quoted(root / "cmake" / "toolchains" / "iOS.cmake");

			/* 包里要装的是「当前工程」：编辑器里打开哪个就配哪个，随包资源跟着走。
			 * 没指定就退回编译期默认工程 —— 每次都显式传，免得构建目录的缓存
			 * 停在上一次的工程上。 */
			const std::filesystem::path effective_project = project_dir.empty()
				? std::filesystem::path(HELIOS_PROJECT_PATH)
				: project_dir;
			const std::string project_arg = " -DHELIOS_PROJECT_DIR=" + Quoted(effective_project);
			CORE_LOG_INFO("[Package] 工程：{}", effective_project.string());

			/* 签名团队号从环境变量带进生成工程：重新生成之后 Xcode 里不用再选一次。
			 * 没设就交给人在 Xcode 里配 */
			std::string team_arg;
			if (const char* team = std::getenv("HELIOS_IOS_DEVELOPMENT_TEAM"); team != nullptr && team[0] != '\0')
				team_arg = " -DHELIOS_IOS_DEVELOPMENT_TEAM=" + std::string(team);

			std::filesystem::path build_dir;
			std::string configure;
			std::string build;

			switch (target)
			{
			case PackageTarget::IOSSimulatorApp:
			{
				build_dir = root / "build-ios-sim";
				configure = cmake + " -S " + Quoted(root) + " -B " + Quoted(build_dir)
					+ " -DCMAKE_TOOLCHAIN_FILE=" + toolchain
					+ " -DHELIOS_IOS_SYSROOT=iphonesimulator"
					+ " -DCMAKE_BUILD_TYPE=Debug -DHELIOS_BUNDLE_ASSETS=ON" + project_arg + team_arg;
				break;
			}
			case PackageTarget::IOSDeviceApp:
			{
				build_dir = root / "build-ios";
				configure = cmake + " -S " + Quoted(root) + " -B " + Quoted(build_dir)
					+ " -DCMAKE_TOOLCHAIN_FILE=" + toolchain
					+ " -DCMAKE_BUILD_TYPE=Release -DHELIOS_BUNDLE_ASSETS=ON" + project_arg + team_arg;
				break;
			}
			case PackageTarget::IOSXcodeProject:
			{
				build_dir = root / "build-ios-xcode";
				ResetBuildDirIfGeneratorDiffers(build_dir, "Xcode");
				configure = cmake + " -S " + Quoted(root) + " -B " + Quoted(build_dir) + " -G Xcode"
					+ " -DCMAKE_TOOLCHAIN_FILE=" + toolchain
					+ " -DCMAKE_BUILD_TYPE=Debug -DHELIOS_BUNDLE_ASSETS=ON" + project_arg + team_arg;
				break;
			}
			}

			std::string log_tail;
			/* CMake 配置阶段不打百分比，进度条走"来回滑"；编译阶段按 [NN%] 走，
			 * 配置算前 15%，编译占剩下的 */
			if (!RunCommand(configure, log_tail, "配置工程", -1.0f, 0.0f))
			{
				result.LogTail = std::move(log_tail);
				return result;
			}

			/* Xcode 工程这一步只配置不出包：出包与签名留给 Xcode 自己 */
			if (target == PackageTarget::IOSXcodeProject)
			{
				ReportProgress("收尾", 1.0f);
				result.OutputPath = FindXcodeProject(build_dir);
				result.Success = !result.OutputPath.empty();
				if (!result.Success)
					result.LogTail = "配置成功但没找到 .xcodeproj";
				return result;
			}

			const unsigned jobs = std::max(2u, std::thread::hardware_concurrency());
			build = cmake + " --build " + Quoted(build_dir) + " --target Player -j " + std::to_string(jobs);
			if (!RunCommand(build, log_tail, "编译", 0.15f, 0.85f))
			{
				result.LogTail = std::move(log_tail);
				return result;
			}

			ReportProgress("收尾", 1.0f);
			result.OutputPath = FindApp(build_dir);
			result.Success = !result.OutputPath.empty();
			if (!result.Success)
				result.LogTail = "构建成功但没找到 .app";
			return result;
		}
	}

	PackageResult Run(PackageTarget target, const std::filesystem::path& project_dir)
	{
		return RunImpl(target, project_dir);
	}

	void StartAsync(PackageTarget target, const std::filesystem::path& project_dir)
	{
		{
			std::lock_guard<std::mutex> lock(s_State.Mutex);
			if (s_State.Running)
				return;

			s_State.Running = true;
			s_State.Label = LabelOf(target);
			s_State.Stage = "准备";
			s_State.Fraction = -1.0f;
			s_State.LastLine.clear();
			s_State.HasPending = false;
		}

		CORE_LOG_INFO("[Package] 开始{}", LabelOf(target));

		/* 裸线程而不是后台任务：打包是几分钟的外部进程，进了 Jobs 会让 Shutdown 卡住退出，
		 * 还长期占一个后台名额。代价是这条线程 detach 之后不受管理 ——
		 * 编辑器退出后它和子进程仍在跑，收尾时会写静态 s_State，此时编辑器已不再读它。 */
		std::thread([target, project_dir]()
			{
				PackageResult result = RunImpl(target, project_dir);

				std::lock_guard<std::mutex> lock(s_State.Mutex);
				s_State.Running = false;
				s_State.Pending = std::move(result);
				s_State.HasPending = true;
			}).detach();
	}

	bool IsRunning()
	{
		std::lock_guard<std::mutex> lock(s_State.Mutex);
		return s_State.Running;
	}

	const char* RunningLabel()
	{
		std::lock_guard<std::mutex> lock(s_State.Mutex);
		return s_State.Label;
	}

	bool GetProgress(Progress& out)
	{
		std::lock_guard<std::mutex> lock(s_State.Mutex);
		if (!s_State.Running)
			return false;

		out.Label = s_State.Label;
		out.Stage = s_State.Stage;
		out.Fraction = s_State.Fraction;
		out.LastLine = s_State.LastLine;
		return true;
	}

	bool CheckEnvironment(std::string& reason)
	{
#if !defined(__APPLE__)
		reason = "iOS 应用只能在 macOS 上打包";
		return false;
#else
		std::error_code error;

		if (!std::filesystem::exists(HELIOS_CMAKE_PATH, error))
		{
			reason = std::string("找不到 CMake：") + HELIOS_CMAKE_PATH;
			return false;
		}

		const std::filesystem::path toolchain =
			EngineRoot() / "cmake" / "toolchains" / "iOS.cmake";
		if (!std::filesystem::exists(toolchain, error))
		{
			reason = "找不到 iOS 工具链文件：" + toolchain.string();
			return false;
		}

		if (!std::filesystem::exists("/usr/bin/xcodebuild", error))
		{
			reason = "没装 Xcode，或命令行工具没配置（xcode-select --install）";
			return false;
		}

		return true;
#endif
	}

	bool Poll(PackageResult& out)
	{
		std::lock_guard<std::mutex> lock(s_State.Mutex);
		if (!s_State.HasPending)
			return false;

		out = std::move(s_State.Pending);
		s_State.HasPending = false;
		return true;
	}

	void RevealInFileManager(const std::string& path)
	{
		if (path.empty())
			return;

#if defined(_WIN32)
		const std::string command = "explorer /select,\"" + path + "\"";
#elif defined(__APPLE__)
		const std::string command = "open -R \"" + path + "\"";
#else
		const std::string command = "xdg-open \"" + path + "\"";
#endif
		if (std::system(command.c_str()) != 0)
			CORE_LOG_WARN("[Package] 无法打开产物目录：{}", path);
	}
}
