#include "Pch.h"
#include "PerfettoWriter.h"
#include "Helios/Profiler/Profiler.h"

namespace Helios
{
	PerfettoWriter& PerfettoWriter::Instance()
	{
		static PerfettoWriter instance;
		return instance;
	}

	void PerfettoWriter::BeginSession(const std::string& name, const std::string& outfile)
	{
		std::lock_guard lock(m_Mutex);

		/* 已有一次会话：先关掉前一次，避免两段事件混在一个文件里 */
		if (m_bSessionActive)
		{
			if (Logger::GetCoreLogger())
				CORE_LOG_ERROR("PerfettoWriter::BeginSession('{0}') when session '{1}' already open.", name, m_SessionName);
			EndSession();
		}

		m_OutputStream.open(outfile);
		if (!m_OutputStream.is_open())
		{
			if (Logger::GetCoreLogger())
				CORE_LOG_ERROR("PerfettoWriter failed to open file {0}.", outfile);
			return;
		}

		m_SessionName = name;
		WriteHeader();
		m_bSessionActive = true;
	}

	void PerfettoWriter::EndSession()
	{
		std::lock_guard lock(m_Mutex);

		m_bSessionActive = false;
		if (!m_OutputStream.is_open())
			return;

		WriteFooter();
		m_OutputStream.close();
		m_SessionName.clear();
	}

	void PerfettoWriter::WriteProfileResult(const ProfileResult& result)
	{
		/* 快径：会话没开直接丢（不构造 json、不抢锁） */
		if (!m_bSessionActive.load(std::memory_order_relaxed))
			return;

		std::stringstream json;
		json << std::setprecision(3) << std::fixed;
		json << ",\n{";
		json << "\"cat\":\"function\",";
		json << "\"dur\":" << (result.ElapsedTime.count()) << ',';
		json << "\"name\":\"" << result.Name << "\",";
		json << "\"ph\":\"X\",";
		json << "\"pid\":0,";
		/* tid 用哈希：libc++ 直接打印 thread::id 是十六进制（0x…），追踪文件就不是合法 JSON 了 */
		json << "\"tid\":" << std::hash<std::thread::id>{}(result.ThreadId) << ",";
		json << "\"ts\":" << result.StartTime.count();
		json << "}";

		std::lock_guard lock(m_Mutex);
		if (!m_OutputStream.is_open())
			return;

		m_OutputStream << json.str();
		m_OutputStream.flush();
	}

	PerfettoWriter::~PerfettoWriter()
	{
		EndSession();
	}

	void PerfettoWriter::WriteHeader()
	{
		m_OutputStream << R"({"otherData": {},"traceEvents":[{})";
		m_OutputStream.flush();
	}

	void PerfettoWriter::WriteFooter()
	{
		m_OutputStream << "\n]}";
		m_OutputStream.flush();
	}

	// PerfettoScope
	PerfettoScope::PerfettoScope(const char* name)
		: m_pName(name)
	{
		/* 写文本全局开关关着：直接空转 —— 不记时间、析构也不提交 */
		if (!PerfettoWriter::Instance().IsSessionActive())
		{
			m_Stopped = true;
			return;
		}

		m_StartTimePoint = std::chrono::steady_clock::now();
	}

	PerfettoScope::~PerfettoScope()
	{
		if (!m_Stopped)
			Stop();
	}

	void PerfettoScope::Stop()
	{
		const auto end_time_point = std::chrono::steady_clock::now();
		const auto high_res_start = std::chrono::duration<double, std::micro>{ m_StartTimePoint.time_since_epoch() };
		const auto elapsed_time = std::chrono::time_point_cast<std::chrono::microseconds>(end_time_point).time_since_epoch()
			- std::chrono::time_point_cast<std::chrono::microseconds>(m_StartTimePoint).time_since_epoch();

		PerfettoWriter::Instance().WriteProfileResult({ m_pName, high_res_start, elapsed_time, std::this_thread::get_id() });

		m_Stopped = true;
	}
}
