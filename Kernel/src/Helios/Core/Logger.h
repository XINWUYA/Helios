#pragma once
#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h>
#include "Helios/Common/Common.h"

namespace Helios 
{
	class HELIOS_API Logger
	{
	public:
		static void Init();
		static void Shutdown();

		inline static SharedPtr<spdlog::logger>& GetCoreLogger() { return s_pCoreLogger; }
		inline static SharedPtr<spdlog::logger>& GetEditorLogger() { return s_pEditorLogger; }

	private:
		static SharedPtr<spdlog::logger> s_pCoreLogger;
		static SharedPtr<spdlog::logger> s_pEditorLogger;
	};

}

// Related macros to simplify
#define CORE_LOG(...)				::Helios::Logger::GetCoreLogger()->trace(__VA_ARGS__)
#define CORE_LOG_INFO(...)			::Helios::Logger::GetCoreLogger()->info(__VA_ARGS__)
#define CORE_LOG_WARN(...)			::Helios::Logger::GetCoreLogger()->warn(__VA_ARGS__)
#define CORE_LOG_ERROR(...)			::Helios::Logger::GetCoreLogger()->error(__VA_ARGS__)
#define CORE_LOG_DEBUG(...)			::Helios::Logger::GetCoreLogger()->debug(__VA_ARGS__)
#define CORE_LOG_CRITICAL(...)		::Helios::Logger::GetCoreLogger()->critical(__VA_ARGS__)

#define EDITOR_LOG(...)				::Helios::Logger::GetEditorLogger()->trace(__VA_ARGS__)
#define EDITOR_LOG_INFO(...)		::Helios::Logger::GetEditorLogger()->info(__VA_ARGS__)
#define EDITOR_LOG_WARN(...)		::Helios::Logger::GetEditorLogger()->warn(__VA_ARGS__)
#define EDITOR_LOG_ERROR(...)		::Helios::Logger::GetEditorLogger()->error(__VA_ARGS__)
#define EDITOR_LOG_DEBUG(...)		::Helios::Logger::GetEditorLogger()->debug(__VA_ARGS__)
#define EDITOR_LOG_CRITICAL(...)	::Helios::Logger::GetEditorLogger()->critical(__VA_ARGS__)
