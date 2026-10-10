#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace Helios
{
	constexpr uint32_t INVALID_CPU_SCOPE_INDEX = UINT32_MAX;

	/* CPU 函数级计时的结果节点（面板 / 录制的数据源；对偶 GPU 的 ResultGPUTimerNode）。
	 * 时间单位微秒：Total = 帧内全部调用的累计时长，Self = 自身代码段
	 * （Total − Σ子节点累计；嵌套 wall time 口径，父 ≠ Σ子，差值就是自身开销）。
	 * Begin / End = 全部调用的首末区间（帧根起算，热力图 / 火焰图用）。 */
	struct ResultCPUTimerNode
	{
		std::string Label{};
		double TotalUs = 0.0;
		double SelfUs = 0.0;
		double BeginUs = 0.0;
		double EndUs = 0.0;
		uint32_t CallCount = 0;
		bool HasData = false;
		std::vector<ResultCPUTimerNode> Children{};
	};

	/* CPU 耗时录制（逐帧回放数据源；语义对偶 GPUTimerRecorder）：每结算一帧追加
	 * 一棵原始值树（不做 EMA），录满自动停、已录内容保留。 */
	class CPUTimerRecorder
	{
	public:
		static constexpr uint32_t MAX_FRAMES = 600;

		/* 开始录制（清空已有内容）；capacity 供检查程序用小容量验证录满自动停 */
		void Start(uint32_t capacity = MAX_FRAMES)
		{
			m_Capacity = capacity < MAX_FRAMES ? capacity : MAX_FRAMES;
			m_Frames.clear();
			m_FrameIds.clear();
			m_Recording = true;
		}

		/* 手动停止（已录内容保留） */
		void Stop() { m_Recording = false; }

		void Clear()
		{
			m_Frames.clear();
			m_FrameIds.clear();
			m_Recording = false;
		}

		bool IsRecording() const { return m_Recording; }
		uint32_t GetFrameCount() const { return static_cast<uint32_t>(m_Frames.size()); }
		uint32_t GetFrameId(uint32_t index) const { return index < m_FrameIds.size() ? m_FrameIds[index] : 0; }
		const ResultCPUTimerNode& GetFrame(uint32_t index) const { return m_Frames[index]; }

		/* 录制回调（结算一帧时调用）：返回本帧的填写目标；
		 * 未在录制或已录满时返回 nullptr（录满的那一帧仍完成填充后自动停止）。 */
		ResultCPUTimerNode* BeginFrame(uint32_t frame_id)
		{
			if (!m_Recording || m_Frames.size() >= m_Capacity)
			{
				m_Recording = false;
				return nullptr;
			}

			m_FrameIds.emplace_back(frame_id);
			m_Frames.emplace_back();
			if (m_Frames.size() >= m_Capacity)
				m_Recording = false;

			return &m_Frames.back();
		}

	private:
		std::vector<ResultCPUTimerNode> m_Frames{};
		std::vector<uint32_t> m_FrameIds{};
		uint32_t m_Capacity{ MAX_FRAMES };
		bool m_Recording{ false };
	};

	/* 函数级 CPU 耗时采集（仅 owner 线程，主线程用）：作用域 RAII 把「标签 + 区间」
	 * 写进预分配节点池，帧末结算 —— EMA 实时树（面板 Live）+ 原始值录制（逐帧回放）。
	 * 对偶 RenderQueryProfiler：GPU 是异步查询，这里是同步区间，模型更简单。 */
	class CPUTimerProfiler
	{
	public:
		static CPUTimerProfiler& Instance();

		void SetEnabled(bool enabled);
		bool IsEnabled() const { return m_Enabled; }

		/* 帧边界：BeginFrame 清池并开帧根作用域（帧号自增）；EndFrame 收尾未闭作用域 + 结算 */
		void BeginFrame();
		void EndFrame();

		/* 作用域开 / 关（ScopedCPUTimerScope 调用）：返回本次调用的池节点下标；
		 * 帧外 / 非 owner 线程时返回 INVALID_CPU_SCOPE_INDEX（调用方静默跳过）。 */
		uint32_t BeginScope(const char* label);
		void EndScope(uint32_t node_index);

		/* 实时树（EMA 平滑；面板 Live 视图的数据源） */
		const ResultCPUTimerNode& GetRealtimeRoot() const { return m_RealtimeRoot; }

		CPUTimerRecorder& GetRecorder() { return m_Recorder; }

	private:
		CPUTimerProfiler() = default;

		/* 帧内作用域节点（池对象跨帧复用）：同一「父 + 标签」的多次调用聚合成一个
		 * 节点 —— 树规模 = 调用图边数，而不是每帧调用次数。标签须为静态字符串。 */
		struct ScopeNode
		{
			const char* Label{ nullptr };
			uint32_t ParentIndex{ INVALID_CPU_SCOPE_INDEX };
			std::vector<uint32_t> ChildrenIndices{};
			double AccumulatedUs{ 0.0 };  /* 累计时长（活跃段首尾之差累加） */
			double BeginUs{ 0.0 };        /* 首末调用的区间（帧根起算） */
			double EndUs{ 0.0 };
			double EntryUs{ 0.0 };        /* 当前活跃段起点（0→1 时记录） */
			uint32_t ActiveCount{ 0 };    /* 未闭合实例数（路径聚合下恒 0/1，留作防御） */
			uint32_t CallCount{ 0 };
		};

		uint32_t AllocNode(const char* label, uint32_t parent_index);
		uint32_t FindChild(uint32_t parent_index, const char* label) const;
		double ElapsedUs() const;

		void SettleRealtime(ResultCPUTimerNode& out, uint32_t index) const; /* EMA */
		void SettleRaw(ResultCPUTimerNode& out, uint32_t index) const;      /* 原始值 */

		bool m_Enabled{ true };
		bool m_FrameBegun{ false };
		uint32_t m_FrameId{ 0 };
		std::thread::id m_OwnerThread{};
		std::chrono::steady_clock::time_point m_FrameStart{};

		std::vector<ScopeNode> m_ScopeNodes{};  /* 帧内节点池（容量复用） */
		uint32_t m_UsedNodeCount{ 0 };
		std::vector<uint32_t> m_OpenIndices{};  /* 打开的作用域栈 */

		ResultCPUTimerNode m_RealtimeRoot{};
		CPUTimerRecorder m_Recorder{};
	};

	/* 作用域 RAII（PROFILE_SCOPE / PROFILE_FUNCTION 宏的落点）：构造进入、析构退出；
	 * 标签须为静态字符串（宏用字面量 / __PRETTY_FUNCTION__ 之类）。 */
	class ScopedCPUTimerScope
	{
	public:
		explicit ScopedCPUTimerScope(const char* label);
		~ScopedCPUTimerScope();

		ScopedCPUTimerScope(const ScopedCPUTimerScope&) = delete;
		ScopedCPUTimerScope& operator=(const ScopedCPUTimerScope&) = delete;

	private:
		uint32_t m_NodeIndex{ INVALID_CPU_SCOPE_INDEX };
	};
}
