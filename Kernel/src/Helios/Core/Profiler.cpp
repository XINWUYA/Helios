#include "Pch.h"
#include "Profiler.h"

namespace Helios
{
	TimeCostProfiler& TimeCostProfiler::Instance()
	{
		static TimeCostProfiler instance;
		return instance;
	}

	void TimeCostProfiler::BeginSession(const std::string& name, const std::string& outfile)
	{
		std::lock_guard lock(m_Mutex);

		if (m_pSessionInfo)  // 如果已有一次会话，先关闭前一次
		{
			if (Logger::GetCoreLogger()) // Edge case: BeginSession() might be before Logger::Init()
				CORE_LOG_ERROR("TimeCostProfiler::BeginSession('{0}') when session '{1}' already open.", name, m_pSessionInfo->Name);
			InternalEndSession();
		}

		m_OutputStream.open(outfile);

		if (m_OutputStream.is_open())
		{
			m_pSessionInfo = CreateSharedPtr<SessionInfo>();
			m_pSessionInfo->Name = name;

			WriteHeader();
			m_bSessionActive = true;
		}
		else
		{
			if (Logger::GetCoreLogger()) // Edge case: BeginSession() might be before Logger::Init()
				CORE_LOG_ERROR("TimeCostProfiler failed to open file {0}.", outfile);
		}
	}

	void TimeCostProfiler::EndSession()
	{
		std::lock_guard lock(m_Mutex);
		InternalEndSession();
	}

	void TimeCostProfiler::WriteProfileResult(const ProfileResult& result)
	{
		/* 快径：会话没开直接丢（写文本关闭时近乎零开销 —— 不构造 json、不抢锁） */
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
		if (m_pSessionInfo)
		{
			m_OutputStream << json.str();
			m_OutputStream.flush();
		}
	}

	TimeCostProfiler::~TimeCostProfiler()
	{
		EndSession();
	}

	void TimeCostProfiler::WriteHeader()
	{
		m_OutputStream << R"({"otherData": {},"traceEvents":[{})";
		m_OutputStream.flush();
	}

	void TimeCostProfiler::WriteFooter()
	{
		m_OutputStream << "\n]}";
		m_OutputStream.flush();
	}

	void TimeCostProfiler::InternalEndSession()
	{
		m_bSessionActive = false;

		if (m_pSessionInfo)
		{
			WriteFooter();
			m_OutputStream.close();

			m_pSessionInfo.reset();
		}
	}

	// *********************************************************************************************
	// ProfilerTimer
	ProfilerTimer::ProfilerTimer(const char* name)
		: m_pName(name)
	{
		/* 写文本全局开关关着：直接空转 —— 不记时间、析构也不提交 */
		if (!TimeCostProfiler::Instance().IsSessionActive())
		{
			m_Stopped = true;
			return;
		}

		m_StartTimePoint = std::chrono::steady_clock::now();
	}

	ProfilerTimer::~ProfilerTimer()
	{
		if (!m_Stopped)
			Stop();
	}

	void ProfilerTimer::Stop()
	{
		const auto end_time_point = std::chrono::steady_clock::now();
		const auto high_res_start = std::chrono::duration<double, std::micro>{ m_StartTimePoint.time_since_epoch() };
		const auto elapsed_time = std::chrono::time_point_cast<std::chrono::microseconds>(end_time_point).time_since_epoch() - std::chrono::time_point_cast<std::chrono::microseconds>(m_StartTimePoint).time_since_epoch();

		TimeCostProfiler::Instance().WriteProfileResult({ m_pName, high_res_start, elapsed_time, std::this_thread::get_id() });

		m_Stopped = true;
	}
}
