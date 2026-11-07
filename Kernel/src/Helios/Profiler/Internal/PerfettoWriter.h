#pragma once
#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include "Helios/Common/Export.h"

namespace Helios
{
	/* Perfetto 追踪事件：一条作用域的起止与所属线程 */
	struct ProfileResult
	{
		std::string Name;
		std::chrono::duration<double, std::micro> StartTime;
		std::chrono::microseconds ElapsedTime;
		std::thread::id ThreadId;
	};

	/* Perfetto JSON 写盘：会话开关加文件流。只服务 HELIOS_PROFILE 分支，
	 * 使用方走 ProfilerTrace 转发，不直接引用这个类。
	 * 事件格式见 https://ui.perfetto.dev/ ，把 json 拖进去即可。 */
	class HELIOS_API PerfettoWriter
	{
	public:
		PerfettoWriter(const PerfettoWriter&) = delete;
		PerfettoWriter(PerfettoWriter&&) = delete;

		static PerfettoWriter& Instance();

		void BeginSession(const std::string& name, const std::string& outfile = "ProfileResult-TimeCost.json");
		void EndSession();

		/* 会话是否在写盘中：关着时 WriteProfileResult 直接返回（不构造 json、不抢锁） */
		[[nodiscard]] bool IsSessionActive() const { return m_bSessionActive.load(std::memory_order_relaxed); }

		void WriteProfileResult(const ProfileResult& result);

	private:
		PerfettoWriter() = default;
		~PerfettoWriter();

		void WriteHeader();
		void WriteFooter();

		std::mutex m_Mutex{};
		std::string m_SessionName{};
		std::ofstream m_OutputStream{};
		std::atomic<bool> m_bSessionActive{ false };
	};
}
