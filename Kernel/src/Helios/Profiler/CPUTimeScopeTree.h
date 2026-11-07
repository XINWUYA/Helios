#pragma once
#include "Helios/Profiler/Profiler.h"

namespace Helios
{
	/* 函数级 CPU 耗时采集（仅 owner 线程，主线程用）：作用域 RAII 把「标签 + 区间」
	 * 写进预分配节点池，帧末结算 —— EMA 实时树（面板 Live）+ 原始值录制（逐帧回放）。
	 * 对偶 GPUTimeScopeTree：GPU 是异步查询，这里是同步区间，模型更简单。 */
	class HELIOS_API CPUTimeScopeTree
	{
	public:
		static CPUTimeScopeTree& Instance();

		void SetEnabled(bool enabled);
		bool IsEnabled() const { return m_Enabled; }

		/* 帧边界：BeginFrame 清池并开帧根作用域（帧号自增）；EndFrame 收尾未闭作用域 + 结算 */
		void BeginFrame();
		void EndFrame();

		/* 等 GPU 结果落地再结算实时树：打开后 EndFrame 只写录制，由 SettleRealtimeFrame 补结算，
		 * 面板上 CPU / GPU 两棵树才是同一帧。等不到就按当前帧兜底，避免树停住。 */
		void SetWaitForGpu(bool wait)
		{
			/* 每帧都会被调用：值没变就别动兜底计数，否则计数永远回零、兜底永不触发 */
			if (m_WaitForGpu == wait)
				return;

			m_WaitForGpu = wait;
			m_FramesSinceSettle = 0;
		}
		void SettleRealtimeFrame(uint32_t frame_id);

		/* 通道掩码：位关掉的插桩点直接跳过（不建节点、不占栈）。
		 * 面板画自己时临时清掉 Editor 位，见 ScopedChannelMask */
		void SetChannelMask(ProfileChannel mask) { m_ChannelMask = mask; }
		ProfileChannel GetChannelMask() const { return m_ChannelMask; }

		/* 作用域开 / 关（CPUTimeScope 调用）：返回本次调用的池节点下标；
		 * 帧外 / 非 owner 线程 / 通道掩码关掉时返回 INVALID_SCOPE_INDEX（调用方静默跳过）。
		 * id = 标签的编译期哈希（同父同 id 聚合）；file / line 可传 nullptr / 0 = 无位置。 */
		uint32_t BeginScope(const char* label, uint64_t id, ProfileChannel channel,
			const char* file, uint32_t line);
		void EndScope(uint32_t node_index);

		/* 实时树（EMA 平滑；面板 Live 视图的数据源） */
		const CPUTimeScopeNode& GetRealtimeRoot() const { return m_RealtimeRoot; }

		/* 当前帧号（帧边界自增；火焰图画布的帧号浮标用） */
		uint32_t GetFrameId() const { return m_FrameId; }

		CPUTimeScopeRecorder& GetRecorder() { return m_Recorder; }

	private:
		CPUTimeScopeTree() = default;

		/* 帧内作用域节点（池对象跨帧复用）：同一「父 + 标签 id」的多次调用聚合成一个，
		 * 树规模只跟调用图边数有关。聚合看 Id，Label 只用来显示；
		 * 节点保留帧内首次出现的插桩点位置。 */
		struct ScopeNode
		{
			const char* Label{ nullptr };
			uint64_t Id{ 0 };
			const char* File{ nullptr };
			uint32_t Line{ 0 };
			ProfileChannel Channel{ ProfileChannel::General };
			uint32_t ParentIndex{ INVALID_SCOPE_INDEX };
			std::vector<uint32_t> ChildrenIndices{};
			double AccumulatedUs{ 0.0 };  /* 累计时长（活跃段首尾之差累加） */
			double BeginUs{ 0.0 };        /* 首末调用的区间（帧根起算） */
			double EndUs{ 0.0 };
			double EntryUs{ 0.0 };        /* 当前活跃段起点（0→1 时记录） */
			uint32_t ActiveCount{ 0 };    /* 未闭合实例数（路径聚合下恒 0/1，留作防御） */
			uint32_t CallCount{ 0 };
		};

		uint32_t AllocNode(const char* label, uint64_t id, const char* file, uint32_t line,
			ProfileChannel channel, uint32_t parent_index);
		uint32_t FindChild(uint32_t parent_index, uint64_t id) const;
		double ElapsedUs() const;

		void SettleRealtime(CPUTimeScopeNode& out, uint32_t index) const; /* EMA */
		void SettleRaw(CPUTimeScopeNode& out, uint32_t index) const;      /* 原始值 */
		/* 把录制里某一帧的原始树并进实时树（等 GPU 时补结算用） */
		static void MergeRealtime(CPUTimeScopeNode& out, const CPUTimeScopeNode& raw);

		bool m_Enabled{ true };
		bool m_FrameBegun{ false };
		bool m_WaitForGpu{ false };
		/* 等 GPU 的兜底：连续这么多帧都没结算就按当前帧结一次（GPU 结果一般晚一两帧） */
		static constexpr uint32_t kSettleTimeout = 3;
		uint32_t m_FramesSinceSettle{ 0 };
		uint32_t m_FrameId{ 0 };
		ProfileChannel m_ChannelMask{ ProfileChannel::All };
		std::thread::id m_OwnerThread{};
		std::chrono::steady_clock::time_point m_FrameStart{};

		std::vector<ScopeNode> m_ScopeNodes{};  /* 帧内节点池（容量复用） */
		uint32_t m_UsedNodeCount{ 0 };
		std::vector<uint32_t> m_OpenIndices{};  /* 打开的作用域栈 */

		CPUTimeScopeNode m_RealtimeRoot{};
		CPUTimeScopeRecorder m_Recorder{};
	};
}
