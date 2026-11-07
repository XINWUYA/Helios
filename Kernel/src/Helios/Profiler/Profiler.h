#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "Helios/Common/Export.h"
#include "Helios/Common/Utils.h"
#include "Internal/ProfileLabel.h"
#if TRACY_ENABLE
#include "tracy/Tracy.hpp"
#include "client/TracyScoped.hpp"
#endif

namespace Helios
{
	constexpr uint32_t INVALID_SCOPE_INDEX = UINT32_MAX;

	/* 采集通道：按子系统过滤插桩点。默认 General；面板画自己时把 Editor
	 * 关掉，统计里就不会混进分析界面本身的开销。位掩码，用下面的运算符组合。 */
	enum class ProfileChannel : uint32_t
	{
		None = 0,
		General = 1u << 0,
		Render = 1u << 1,
		Scene = 1u << 2,
		Editor = 1u << 3,
		UI = 1u << 4,
		Assets = 1u << 5,
		Jobs = 1u << 6,
		All = 0xFFFFFFFFu
	};

	/* 启用 Common/Utils.h 里的通用位掩码运算符，不再各写一套 */
	template <> struct EnableBitmaskOperators<ProfileChannel> : std::true_type {};

	/* 掩码里是否开了某个通道（组合位任一位命中即为真） */
	constexpr bool HasChannel(ProfileChannel mask, ProfileChannel channel)
	{
		return (mask & channel) != ProfileChannel::None;
	}

	/* 数据出口：一次埋点可以同时喂给多个出口，各自判自己那一位。
	 * Tracy 需要在构建时带进来，没编进来时那一位是空操作。 */
	enum class ProfileSink : uint32_t
	{
		None = 0,
		Panel = 1u << 0,
		Perfetto = 1u << 1,
		Tracy = 1u << 2,
		All = 0xFFFFFFFFu
	};

	template <> struct EnableBitmaskOperators<ProfileSink> : std::true_type {};

	/* 出口掩码里是否开了某一位 */
	constexpr bool HasSink(ProfileSink mask, ProfileSink sink)
	{
		return (mask & sink) != ProfileSink::None;
	}

	/* CPU 作用域结果节点。时间单位微秒：Total = 帧内全部调用的累计时长，
	 * Self = 自身代码段（Total − Σ子节点累计），Begin / End = 全部调用的首末区间。 */
	struct CPUTimeScopeNode
	{
		std::string Label{};
		/* 插桩点位置（悬浮读数显示「文件名:行号」）：File 指向静态存储、Line 0 = 未知 */
		const char* File{ nullptr };
		uint32_t Line{ 0 };
		double TotalUs{ 0.0 };
		double SelfUs{ 0.0 };
		double BeginUs{ 0.0 };
		double EndUs{ 0.0 };
		uint32_t CallCount{ 0 };
		bool HasData{ false };
		/* 采集通道：面板按它筛选子树（GPU 节点没有通道，恒 General） */
		ProfileChannel Channel{ ProfileChannel::General };
		std::vector<CPUTimeScopeNode> Children{};
	};

	/* GPU 作用域结果节点。Total = 绑定通道的阶段窗口并集，Self 由总耗时减子项推导；
	 * 没有起止时间，火焰图按提交顺序铺。
	 * 阶段细分只有通道采样的后端给值（Metal），其余恒 0，看 HasStageSplit。 */
	struct GPUTimeScopeNode
	{
		std::string Label{};
		double TotalUs{ 0.0 };
		double SelfUs{ 0.0 };
		bool HasData{ false };
		double StageVertexUs{ 0.0 };
		double StageFragmentUs{ 0.0 };
		double StageComputeUs{ 0.0 };
		bool HasStageSplit{ false };
		uint32_t QueryIndex{ INVALID_SCOPE_INDEX };
		std::vector<GPUTimeScopeNode> Children{};
	};

	/* 逐帧录制：默认常开滚动，环形覆盖最近 MAX_FRAMES 帧的原始值树（不做 EMA），
	 * 卡顿发生时不必事先点录制。CPU 与 GPU 共用这份实现，节点类型各自一套。
	 * Start(capacity) 切到定长捕获（录满即停），供检查程序与基准测试按固定帧数取样。 */
	template <typename NodeT>
	class TimeScopeRecorderT
	{
	public:
		static constexpr uint32_t MAX_FRAMES = 600;
		static constexpr uint32_t INVALID_FRAME_INDEX = UINT32_MAX;

		TimeScopeRecorderT()
		{
			m_Frames.resize(MAX_FRAMES);
			m_FrameIds.resize(MAX_FRAMES);
		}

		/* 定长捕获：清空并录满即停（检查程序用小容量验证语义） */
		void Start(uint32_t capacity = MAX_FRAMES)
		{
			m_Capacity = capacity > 0 && capacity < MAX_FRAMES ? capacity : MAX_FRAMES;
			m_Write = 0;
			m_Count = 0;
			m_Rolling = false;
			m_Recording = true;
		}

		/* 停止 / 继续采集（内容保留） */
		void Stop() { m_Recording = false; }
		void Resume() { m_Recording = true; }

		/* 清空内容：采集状态不变（滚动窗口清空后继续录） */
		void Clear()
		{
			m_Write = 0;
			m_Count = 0;
		}

		bool IsRecording() const { return m_Recording; }
		uint32_t GetFrameCount() const { return m_Count; }
		uint32_t GetFrameId(uint32_t index) const { return index < m_Count ? m_FrameIds[Physical(index)] : 0; }
		const NodeT& GetFrame(uint32_t index) const { return m_Frames[Physical(index)]; }

		/* 按引擎帧号找槽位（滚动窗口里可能已被覆盖）；找不到返回 INVALID_FRAME_INDEX */
		uint32_t FindIndexByFrameId(uint32_t frame_id) const
		{
			for (uint32_t index = 0; index < m_Count; ++index)
			{
				if (m_FrameIds[Physical(index)] == frame_id)
					return index;
			}
			return INVALID_FRAME_INDEX;
		}

		/* 录制回调（结算一帧时调用）：返回本帧的填写目标；
		 * 暂停或定长捕获已满时返回 nullptr（填满的那一帧仍完成填充后自动停止）。 */
		NodeT* BeginFrame(uint32_t frame_id)
		{
			if (!m_Recording)
				return nullptr;

			if (!m_Rolling && m_Count >= m_Capacity)
			{
				m_Recording = false;
				return nullptr;
			}

			const uint32_t slot = m_Write;
			m_FrameIds[slot] = frame_id;
			m_Write = (m_Write + 1) % m_Capacity;
			if (m_Count < m_Capacity)
				++m_Count;

			if (!m_Rolling && m_Count >= m_Capacity)
				m_Recording = false;

			return &m_Frames[slot];
		}

	private:
		/* 逻辑下标（0 = 最旧）到环形槽位 */
		uint32_t Physical(uint32_t index) const
		{
			return (m_Write + m_Capacity - m_Count + index) % m_Capacity;
		}

		std::vector<NodeT> m_Frames{};
		std::vector<uint32_t> m_FrameIds{};
		uint32_t m_Capacity{ MAX_FRAMES };
		uint32_t m_Write{ 0 };
		uint32_t m_Count{ 0 };
		bool m_Rolling{ true };
		bool m_Recording{ true };
	};

	using CPUTimeScopeRecorder = TimeScopeRecorderT<CPUTimeScopeNode>;
	using GPUTimeScopeRecorder = TimeScopeRecorderT<GPUTimeScopeNode>;

	/* 帧阶段划分（不渲染的帧不统计） */
	enum class FrameStage : uint8_t
	{
		Update = 0,  /* 逻辑更新与渲染提交 */
		UI,          /* ImGui 界面更新与绘制数据生成 */
		Present,     /* 交换 / 提交显示（含事件轮询） */
		Count
	};
	constexpr uint32_t FRAME_STAGE_COUNT = static_cast<uint32_t>(FrameStage::Count);

	/* 帧耗时快照（门面每帧从 CPU / GPU 结果树派生，面板只读） */
	struct FrameTimeSnapshot
	{
		float FrameMs{ 0.0f };                  /* 帧间隔，含垂直同步等待 */
		float CPUMs{ 0.0f };                    /* 主线程工作合计 */
		float StageMs[FRAME_STAGE_COUNT]{};     /* 各阶段耗时（无数据时保留上一次的平滑值） */
		bool StageValid[FRAME_STAGE_COUNT]{};   /* 该阶段本帧是否有采集；false = 画占位 */
		float GPUMs{ 0.0f };                    /* GPU 总耗时（GPU 树根） */
		bool GPUValid{ false };                 /* GPU 计时是否可用 */
	};

	/* 帧历史样本（原始值，供面板画曲线） */
	struct FrameTimeSample
	{
		float FrameMs{ 0.0f };
		float CPUMs{ 0.0f };
		float GPUMs{ 0.0f };
		bool GPUValid{ false };
	};

	/* 一帧的渲染统计快照：提交次数与几何吞吐量（面板只读，不参与任何渲染决策）。
	 * 口径 —— 顶点 / 三角形按「这一笔提交了多少元素」折算，实例化乘实例数；
	 * 间接绘制（草地）的元素与实例数在 GPU 侧，只计入调用数，几何量不计。
	 * 草的提交按通道分三类：同一份剔除结果会被多个 Pass 各提交一遍，混在一起看不出是谁。 */
	struct RenderStatisticsSnapshot
	{
		uint64_t DrawCalls{ 0 };
		uint64_t Triangles{ 0 };
		uint64_t Vertices{ 0 };
		uint64_t Instances{ 0 };
		uint64_t IndirectDrawCalls{ 0 };
		uint64_t GrassVisibleDrawCalls{ 0 };
		uint64_t GrassShadowDrawCalls{ 0 };
		uint64_t GrassVelocityDrawCalls{ 0 };
		bool Valid{ false };
	};

	/* CPU 作用域 RAII：构造进入、析构结算。
	 * 标签须指向静态存储（宏经 PROFILE_LABEL_LINE 裁剪并落 static constexpr）。
	 * 带 id 的重载是宏走的路：id 由编译期哈希给出，热路径不做字符串比较。 */
	class HELIOS_API CPUTimeScope
	{
	public:
		CPUTimeScope(const char* label, uint64_t id, ProfileChannel channel,
			const char* file, uint32_t line);
		CPUTimeScope(const char* label, const char* file, uint32_t line);
		~CPUTimeScope();

		CPUTimeScope(const CPUTimeScope&) = delete;
		CPUTimeScope& operator=(const CPUTimeScope&) = delete;

	private:
		uint32_t m_NodeIndex{ INVALID_SCOPE_INDEX };
	};

	/* GPU 计时作用域：构造时压调试分组并开计时节点，析构时弹出，中途 return 也不会漏掉配对。
	 * 和 CPUTimeScope 分成两个类，是因为一个量 CPU 墙钟区间，一个只是提交分组，
	 * 真正的耗时来自后端的异步查询，两者的生命周期跟着不同的东西走。 */
	class HELIOS_API GPUTimeScope final
	{
	public:
		explicit GPUTimeScope(const char* label);
		explicit GPUTimeScope(const std::string& label);
		~GPUTimeScope();

		GPUTimeScope(const GPUTimeScope&) = delete;
		GPUTimeScope& operator=(const GPUTimeScope&) = delete;

	private:
		bool m_Active{ false };
	};

	/* 临时改采集通道掩码（析构恢复）：面板画自己时清掉 Editor 位，
	 * 统计里不混进分析界面本身的开销 */
	class ScopedChannelMask
	{
	public:
		explicit ScopedChannelMask(ProfileChannel mask);
		/* 指定析构时恢复成哪个掩码：面板画完要恢复成用户的选择，
		 * 不能恢复成进入前的值（那样每帧都绕回原样，筛选永远不生效） */
		ScopedChannelMask(ProfileChannel mask, ProfileChannel restore);
		~ScopedChannelMask();

		ScopedChannelMask(const ScopedChannelMask&) = delete;
		ScopedChannelMask& operator=(const ScopedChannelMask&) = delete;

	private:
		ProfileChannel m_RestoreMask;
	};

	/* Perfetto 出口的开关转发：宏与面板都从这里进，不直接依赖写盘实现。
	 * 实现在 Internal/PerfettoWriter.cpp，会话关着时所有调用都是空操作。 */
	namespace ProfilerTrace
	{
		[[nodiscard]] HELIOS_API bool IsSessionActive();
		HELIOS_API void BeginSession(const char* name, const char* file);
		HELIOS_API void EndSession();
	}

	/* 性能计时门面：帧边界、采集开关、数据读取都从这里进。
	 * 使用方只 include 这个头，不碰内部采集器；将来加内存维度也是在这里开口子。 */
	class HELIOS_API Profiler
	{
	public:
		static Profiler& Instance();

		/* 帧边界：主循环每帧一对（最小化等不渲染的帧不调用）。
		 * BeginFrame 记帧间隔（含垂直同步等待）；BeginCPUTiming 在等待之后开 CPU 采集，
		 * 两段口径不同，所以不合成一个调用。 */
		void BeginFrame();
		void BeginCPUTiming();
		void EndFrame();

		/* 渲染器末尾：收掉 GPU 查询、解析结果、结算几何统计，然后开下一帧的 GPU 采集 */
		void EndGPUFrame();
		void BeginGPUFrame(uint32_t frame_id);

		void SetCPUEnabled(bool enabled);
		void SetGPUEnabled(bool enabled);
		[[nodiscard]] bool IsCPUEnabled() const;
		[[nodiscard]] bool IsGPUEnabled() const;

		/* 通道筛选：位没开的插桩点直接跳过 */
		void SetChannelFilter(ProfileChannel channels);
		[[nodiscard]] ProfileChannel GetChannelFilter() const;

		/* 等 GPU 结果落地再结算 CPU 实时树：打开后面板上两棵树是同一帧 */
		void SetWaitForGPU(bool wait);

		/* 实时树（EMA 平滑） */
		[[nodiscard]] const CPUTimeScopeNode& GetCPURoot() const;
		[[nodiscard]] const GPUTimeScopeNode& GetGPURoot() const;

		/* 帧快照与历史曲线 */
		[[nodiscard]] const FrameTimeSnapshot& GetFrameTime() const;
		[[nodiscard]] uint32_t GetFrameHistoryCount() const;
		[[nodiscard]] const FrameTimeSample& GetFrameHistorySample(uint32_t index) const;
		[[nodiscard]] const RenderStatisticsSnapshot& GetRenderStatistics() const;

		/* 录制控制：暂停只停追加、内容保留，清空只清内容；两路一起控制
		 * （面板的暂停与清空对 CPU / GPU 同时生效） */
		void SetRecording(bool recording);
		void SetCPURecording(bool recording);
		void SetGPURecording(bool recording);
		[[nodiscard]] bool IsRecording() const;
		void ClearRecording();

		/* 逐帧录制：按索引取树、按帧号反查索引（找不到返回 INVALID_SCOPE_INDEX） */
		[[nodiscard]] uint32_t GetCPUFrameCount() const;
		[[nodiscard]] const CPUTimeScopeNode& GetCPUFrame(uint32_t index) const;
		[[nodiscard]] uint32_t GetCPUFrameId(uint32_t index) const;
		[[nodiscard]] uint32_t FindCPUFrame(uint32_t frame_id) const;

		/* 录制器直读：给导出这类要遍历原始帧的场景；面板显示走上面的按帧 API */
		[[nodiscard]] const GPUTimeScopeRecorder& GetGPURecorder() const;

		[[nodiscard]] uint32_t GetGPUFrameCount() const;
		[[nodiscard]] const GPUTimeScopeNode& GetGPUFrame(uint32_t index) const;
		[[nodiscard]] uint32_t GetGPUFrameId(uint32_t index) const;
		[[nodiscard]] uint32_t FindGPUFrame(uint32_t frame_id) const;

		/* 历史窗口长度（面板画曲线的上限） */
		static constexpr uint32_t HISTORY_SIZE = 120;

		/* 手动登记一帧的 GPU 结果：正常由 EndGPUFrame 调用，
		 * 自检里用它注入确定值，验证两半一起发布的时序 */
		void PublishGPU(uint32_t frame_id, bool gpu_valid, float gpu_ms);

		/* 直接推一帧样本：不碰采集器、不碰帧边界，只更新快照和历史。
		 * 图表测试造曲线走这里，才不会和真实采集抢状态。 */
		void PushFrameSample(float frame_ms, const float (&stage_ms)[FRAME_STAGE_COUNT],
			bool gpu_valid, float gpu_ms);

		/* 帧阶段：数量和显示名（面板画阶段行用） */
		static constexpr uint32_t STAGE_COUNT = FRAME_STAGE_COUNT;
		static const char* GetStageName(FrameStage stage);

		/* 当前 CPU 采集帧号；GPU 结果所属帧号（查询异步，通常晚一两帧） */
		[[nodiscard]] uint32_t GetFrameId() const;
		[[nodiscard]] uint32_t GetGPUResultFrameId() const;

	private:
		Profiler() = default;

		/* 等 GPU 期间的待发布帧：CPU 侧的数先存着，GPU 结果到了再一起结算 */
		struct PendingFrame
		{
			uint32_t FrameId{ 0 };
			float FrameMs{ 0.0f };
			float StageMs[FRAME_STAGE_COUNT]{};
			bool StageSeen[FRAME_STAGE_COUNT]{};
			bool Used{ false };
		};
		static constexpr uint32_t kPendingCount = 8;
		/* 等不到 GPU 结果时的兜底：超过这么多帧还没等到就按"无 GPU 数据"发布 */
		static constexpr uint32_t kPendingTimeout = 4;
		static constexpr uint32_t kHistorySize = HISTORY_SIZE;

		void PublishFrame(PendingFrame& frame, bool gpu_valid, float gpu_ms);
		[[nodiscard]] PendingFrame* FindPending(uint32_t frame_id);
		/* 阶段耗时从本帧录制里取：等 GPU 时实时树不结算，从树里读会拿到上一帧 */
		static void ReadStages(const CPUTimeScopeNode& frame,
			float (&stage_ms)[FRAME_STAGE_COUNT], bool (&seen)[FRAME_STAGE_COUNT]);

		bool m_FrameBegun{ false };
		bool m_WaitForGpu{ false };
		std::chrono::steady_clock::time_point m_FrameStart{};

		PendingFrame m_PendingFrames[kPendingCount]{};
		FrameTimeSnapshot m_Snapshot{};
		bool m_HasSnapshot{ false };
		FrameTimeSample m_History[kHistorySize]{};
		uint32_t m_HistoryWrite{ 0 };
		uint32_t m_HistoryCount{ 0 };
	};

	// 分析计时器
	class HELIOS_API PerfettoScope
	{
	public:
		PerfettoScope(const char* name);
		~PerfettoScope();

		void Stop();

	private:
		const char* m_pName;
		std::chrono::time_point<std::chrono::steady_clock> m_StartTimePoint{};
		bool m_Stopped{ false };
	};


}

/* 追踪文本拖进 edge://tracing 或 ui.perfetto.dev 就能看。HELIOS_PROFILE 是它的编译期开关：
 * 0 不编译计时器、会话调用为空操作，1 才编译，写不写盘另由面板 Trace Text 控制。
 * 面板计时采集不受影响，标签走同一套 PROFILE_LABEL_LINE；编辑器在 CMakeLists 定义 =1。 */
#ifndef HELIOS_PROFILE
	#define HELIOS_PROFILE 0
#endif

// Resolve which function signature macro will be used. Note that this only
// is resolved when the (pre)compiler starts, so the syntax highlighting
// could mark the wrong one in your editor!
#if defined(__GNUC__) || (defined(__MWERKS__) && (__MWERKS__ >= 0x3000)) || (defined(__ICC) && (__ICC >= 600)) || defined(__ghs__)
	#define FUNC_SIG __PRETTY_FUNCTION__
#elif defined(__DMC__) && (__DMC__ >= 0x810)
	#define FUNC_SIG __PRETTY_FUNCTION__
#elif (defined(__FUNCSIG__) || (_MSC_VER))
	#define FUNC_SIG __FUNCSIG__
#elif (defined(__INTEL_COMPILER) && (__INTEL_COMPILER >= 600)) || (defined(__IBMCPP__) && (__IBMCPP__ >= 500))
	#define FUNC_SIG __FUNCTION__
#elif defined(__BORLANDC__) && (__BORLANDC__ >= 0x550)
	#define FUNC_SIG __FUNC__
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901)
	#define FUNC_SIG __func__
#elif defined(__cplusplus) && (__cplusplus >= 201103)
	#define FUNC_SIG __func__
#else
	#define FUNC_SIG "FUNC_SIG unknown!"
#endif

/* 标签准备：编译期把编译器签名裁成「类::函数名」，并同时算一个哈希 id ——
	 * 采集合聚按 id 比较，热路径不再比字符串。都落 static：采集器只存指针、
	 * 帧末结算时才读内容（函数局部的 constexpr 在 -O0 下会悬垂）。 */
	#define PROFILE_LABEL_LINE(name, line) static constexpr auto fixed_name_##line = ::Helios::ProfilerUtils::CleanupFunctionLabel(name); \
											static constexpr uint64_t fixed_id_##line = ::Helios::ProfileHash::Hash(fixed_name_##line.Data)

	/* 追踪文本（Perfetto）：一个作用域额外挂一个计时器，会话关着时空转 */
	#if HELIOS_PROFILE
		// 开启一个耗时分析（写追踪文本；实际写不写由运行期全局开关控制）
		#define PROFILER_BEGIN_SESSION(name, filepath) ::Helios::ProfilerTrace::BeginSession(name, filepath)
		// 关闭一个耗时分析
		#define PROFILER_END_SESSION() ::Helios::ProfilerTrace::EndSession()
		#define PROFILE_TRACE_SCOPE_LINE(name, line) ::Helios::PerfettoScope trace_##line(name)
	#else
		#define PROFILER_BEGIN_SESSION(name, filepath)
		#define PROFILER_END_SESSION()
		#define PROFILE_TRACE_SCOPE_LINE(name, line)
	#endif

	/* Tracy：编译期带进来时并行开一个 zone。用 ZoneNamedN 而不是 ZoneScopedN ——
	 * 后者的变量名固定，同一个作用域里写两个就会重定义。 */
	#if TRACY_ENABLE
		#define PROFILE_TRACY_SCOPE_LINE(name, line) ZoneNamedN(tracy_zone_##line, name, true)
	#else
		#define PROFILE_TRACY_SCOPE_LINE(name, line)
	#endif

	/* 一处埋点，三个消费者：面板采集常开，追踪文本看 HELIOS_PROFILE，Tracy 看 TRACY_ENABLE。
	 * 各自在自己的宏里判开关，调用点只写一次；展开成声明，须处于语句上下文（带分号）。 */
	#define PROFILE_CPU_SCOPE_CH_LINE(channel, name, file, line) PROFILE_LABEL_LINE(name, line); \
											PROFILE_TRACE_SCOPE_LINE(fixed_name_##line.Data, line); \
											const ::Helios::CPUTimeScope helios_cpu_scope_##line(fixed_name_##line.Data, fixed_id_##line, channel, file, line); \
											PROFILE_TRACY_SCOPE_LINE(fixed_name_##line.Data, line)

	/* 行号 / 文件名先经这层"普通转发"展开再进粘连与调用 —— `##` 会抑制参数展开，
	 * __LINE__ 直接进 PROFILE_CPU_SCOPE_CH_LINE 会以字面 token 参与粘连（名字糊成
	 * <前缀>___LINE__）；__FILE__ 同层展开成字符串字面量（插桩点文件，悬浮读数用）。 */
	#define PROFILE_CPU_SCOPE_CH_LINE_EXPAND(channel, name, line) PROFILE_CPU_SCOPE_CH_LINE(channel, name, __FILE__, line)
	#define PROFILE_CPU_SCOPE_CH(channel, name) PROFILE_CPU_SCOPE_CH_LINE_EXPAND(channel, name, __LINE__)
	/* 默认通道 General：不标注的插桩点照旧全收，需要按子系统过滤时用 _CH 版本 */
	#define PROFILE_CPU_SCOPE(name) PROFILE_CPU_SCOPE_CH(::Helios::ProfileChannel::General, name)
	// 分析当前函数耗时
	#define PROFILE_CPU_FUNCTION() PROFILE_CPU_SCOPE(FUNC_SIG)
	#define PROFILE_CPU_FUNCTION_CH(channel) PROFILE_CPU_SCOPE_CH(channel, FUNC_SIG)

/* GPU 计时作用域：把这一段工作单独挂到计时树的下一层，用法和 PROFILE_CPU_SCOPE 一致。
 * 行号拼变量名，同一作用域里可以写多次。不受 Tracy 开关影响 —— 它只推调试分组，
 * 走的是 RenderAPI，不是 Tracy 的客户端队列。 */
#define PROFILE_GPU_SCOPE_LINE(label, line) const ::Helios::GPUTimeScope gpu_scope_##line(label)
#define PROFILE_GPU_SCOPE_LINE_EXPAND(label, line) PROFILE_GPU_SCOPE_LINE(label, line)
#define PROFILE_GPU_SCOPE(label) PROFILE_GPU_SCOPE_LINE_EXPAND(label, __LINE__)
