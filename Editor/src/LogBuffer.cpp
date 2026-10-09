#include "Pch.h"
#include "LogBuffer.h"

#include <chrono>
#include <spdlog/sinks/base_sink.h>

#include "Helios/Core/Logger.h"

namespace Helios
{
	namespace
	{
		/* 捕获 sink：把每条日志的展示要素（时间 / 级别 / 来源 / 正文）原样塞进
		 * LogBuffer。不走 sink 的 formatter —— msg.payload 就是调用点格式化好的
		 * 正文，时间戳、级别标、来源名单独存字段（面板自己排版）。 */
		class LogCaptureSink final : public spdlog::sinks::base_sink<std::mutex>
		{
		protected:
			void sink_it_(const spdlog::details::log_msg& msg) override
			{
				const auto time_point = std::chrono::time_point_cast<std::chrono::milliseconds>(msg.time);
				const std::time_t seconds = std::chrono::system_clock::to_time_t(time_point);
				const auto millis = static_cast<uint32_t>(
					std::chrono::duration_cast<std::chrono::milliseconds>(time_point.time_since_epoch()).count() % 1000);

				LogBuffer::Instance().Push(static_cast<int>(msg.level),
					std::string_view(msg.logger_name.data(), msg.logger_name.size()),
					std::string_view(msg.payload.data(), msg.payload.size()),
					seconds, millis);
			}

			void flush_() override {}
		};
	}

	LogBuffer& LogBuffer::Instance()
	{
		static LogBuffer instance;
		return instance;
	}

	void LogBuffer::Push(int level, std::string_view logger_name, std::string_view message,
		std::time_t seconds, uint32_t millis)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);

		Record& record = m_Records.emplace_back();
		record.Seq = m_NextSeq++;
		record.Seconds = seconds;
		record.Millis = millis;
		record.Level = level;
		record.Logger.assign(logger_name);
		record.Message.assign(message);

		while (m_Records.size() > kCapacity)
			m_Records.pop_front();
	}

	bool LogBuffer::FetchSince(uint64_t& since, std::vector<Record>& out)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		out.clear();

		if (m_Records.empty())
			return true; /* 无存活条目（含刚被清空）：调用方清缓存收口 */

		/* 整体重建的两种情形：since 落点已被环形覆盖；或 since 早于最近一次
		 * Clear（旧游标对应的条目已被清空，继续增量会让清空前的缓存残留） */
		const bool rebuild = (since + 1 < m_Records.front().Seq) || (since < m_ClearedBelow);
		for (const Record& record : m_Records)
		{
			if (record.Seq > since)
				out.push_back(record);
		}

		if (!out.empty())
			since = out.back().Seq;

		return rebuild;
	}

	void LogBuffer::Clear()
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Records.clear();
		m_ClearedBelow = m_NextSeq;
	}

	uint32_t LogBuffer::GetCount() const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		return static_cast<uint32_t>(m_Records.size());
	}

	void AttachLogCapture()
	{
		static bool attached = false;
		if (attached)
			return;
		attached = true;

		/* sink 由捕获通道自持：logger 的 sink 列表是 SharedPtr 数组，挂上后随 logger 存活 */
		static const SharedPtr<LogCaptureSink> capture = CreateSharedPtr<LogCaptureSink>();
		Logger::GetCoreLogger()->sinks().push_back(capture);
		Logger::GetEditorLogger()->sinks().push_back(capture);
	}
}
