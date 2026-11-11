#include "Pch.h"
#include "Logger.h"
#include "Helios/Common/AssetPath.h"
#include <cstdio>
#include <cstdlib>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

namespace Helios
{
	namespace
	{
		/* 依次尝试「cwd/Helios.log」（终端工作流的位置）和用户日志目录；都打不开返回空
		 * （退化成纯控制台日志）。双击 .app 启动时 cwd 是「/」写不进去，且工程目录受 TCC
		 * 保护（会弹授权框卡住启动），所以退到用户日志目录。 */
		spdlog::sink_ptr CreateFileSink()
		{
			std::vector<std::filesystem::path> candidates;
			candidates.emplace_back("Helios.log");

			const std::filesystem::path log_dir = GetWritableLogDirectory();
			if (!log_dir.empty())
			{
				std::error_code error;
				std::filesystem::create_directories(log_dir, error);
				if (!error)
					candidates.push_back(log_dir / "Helios.log");
			}

			for (const std::filesystem::path& path : candidates)
			{
				try
				{
					return CreateSharedPtr<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
				}
				catch (const spdlog::spdlog_ex&)
				{
					/* 换下一个候选，全部失败由调用方降级 */
				}
			}
			return nullptr;
		}
	}

	SharedPtr<spdlog::logger> Logger::s_pCoreLogger;
	SharedPtr<spdlog::logger> Logger::s_pEditorLogger;

	void Logger::Init()
	{
		std::vector<spdlog::sink_ptr> log_sinks;
		log_sinks.emplace_back(CreateSharedPtr<spdlog::sinks::stdout_color_sink_mt>());

		/* 文件打不开就退纯控制台 —— 不能反过来把进程带崩 */
		if (spdlog::sink_ptr file_sink = CreateFileSink())
		{
			log_sinks.push_back(std::move(file_sink));
		}
		else
		{
			std::fprintf(stderr, "[Logger] File log disabled.\n");
		}

		// 指定Log样式
		log_sinks[0]->set_pattern("%^[%T] [thread %t] %n: %v%$");
		if (log_sinks.size() > 1)
			log_sinks[1]->set_pattern("[%T] [thread %t] [%l] %n: %v");

		s_pCoreLogger = CreateSharedPtr<spdlog::logger>("Kernel", std::begin(log_sinks), std::end(log_sinks));
		spdlog::register_logger(s_pCoreLogger);
		s_pCoreLogger->set_level(spdlog::level::trace);
		s_pCoreLogger->flush_on(spdlog::level::trace);

		s_pEditorLogger = CreateSharedPtr<spdlog::logger>("Editor", std::begin(log_sinks), std::end(log_sinks));
		spdlog::register_logger(s_pEditorLogger);
		s_pEditorLogger->set_level(spdlog::level::trace);
		s_pEditorLogger->flush_on(spdlog::level::trace);

	}

	void Logger::Shutdown()
	{
		spdlog::shutdown();
	}
}
