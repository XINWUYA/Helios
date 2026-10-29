#pragma once
#include <cstdint>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Helios
{
	/* 日志捕获缓冲：spdlog 捕获 sink 和日志窗口之间的唯一通道（进程级单例）。写入端可以在任意
	 * 线程（Push 自带锁）；读取端每帧增量取新（FetchSince）。容量 = 固定条数的环形缓冲，序号单调
	 * 递增；环形覆盖 / 清空都由 FetchSince 返回的"整体重建"收口。 */
	class LogBuffer
	{
	public:
		/* 环形缓冲容量：够回溯一整段会话，又不至于让每帧的增量拷贝变重 */
		static constexpr uint32_t kCapacity = 2048;

		struct Record
		{
			uint64_t    Seq{ 0 };      /* 单调递增序号（增量取新的游标） */
			std::time_t Seconds{ 0 };  /* 本地时间（秒） */
			uint32_t    Millis{ 0 };   /* 毫秒补位 */
			int         Level{ 0 };    /* spdlog::level::level_enum */
			std::string Logger;        /* 来源 logger（Kernel / Editor） */
			std::string Message;       /* 正文（fmt 已格式化） */
		};

		static LogBuffer& Instance();

		/* 日志线程调用：追加一条（超出容量丢最旧） */
		void Push(int level, std::string_view logger_name, std::string_view message,
			std::time_t seconds, uint32_t millis);

		/* UI 线程调用：取 since 之后的新条目（先清空 out）。返回值 = 要不要整体重建（环形缓冲覆盖了
		 * since，或者 since 早于最近一次 Clear —— 这时 out 是全部存活条目）；成功后 since 往前推。 */
		bool FetchSince(uint64_t& since, std::vector<Record>& out);

		/* 清空存活条目。序号不回退：清空后所有旧游标的取数都触发整体重建 */
		void Clear();

		/* 当前存活条目数（面板计数显示用） */
		uint32_t GetCount() const;

	private:
		mutable std::mutex m_Mutex;
		std::deque<Record> m_Records;
		uint64_t m_NextSeq{ 1 };     /* 序号只增不减：清空后新条目仍大于旧游标 */
		uint64_t m_ClearedBelow{ 0 }; /* 最近一次 Clear 时已分配的序号水位（见 FetchSince） */
	};

	/* 把日志窗口的捕获 sink 挂到 Kernel / Editor 两个 logger 上。
	 * 幂等；在 Logger::Init() 之后调用一次（编辑器装配层）。 */
	void AttachLogCapture();
}
