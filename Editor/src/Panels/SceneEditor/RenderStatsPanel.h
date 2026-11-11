#pragma once
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>
#include "Common/EditorCommon.h"
#include "Helios/Profiler/Profiler.h"
#include "EditorUI/Widgets/SlidingTabs.h"
#include "EditorUI/Widgets/TimingChart.h"

namespace Helios
{
	/* 「Render Stats」面板：每帧 CPU / GPU 性能统计。三张卡 —— Frame（FPS / 帧间隔 /
	 * CPU 阶段 / GPU 合计 + 三线历史曲线）、Timings（一套 Live / 录制回放，树按 CPU / GPU
	 * 源页签切换，共用一条时间轴）和 Geometry（提交次数与几何吞吐）。
	 * 数据源 = FrameTimeProfiler + CPUTimeScopeTree + GPUTimeScopeTree + RenderStatistics。 */
	class RenderStatsPanel
	{
	public:
		/* 用户选的采集通道：渲染时临时去掉 Editor 位，析构恢复由 ScopedChannelMask 负责，
		 * 所以选择必须存在面板自己这里，不能被那层 RAII 覆盖 */
		ProfileChannel m_ChannelFilter{ ProfileChannel::All };
		float m_SwitcherWidth{ 0.0f };

		/* 图表状态在共享组件里，面板只保留别名 */
		using TimingChartView = TimingChart::ChartView;
		using TimingViewState = TimingChart::ViewState;

		/* 计时卡看哪棵树：两条采集都在后台常开，这里只决定显示哪个 */
		enum class TimingSource : uint8_t
		{
			CPU = 0,
			GPU
		};

		/* 画整个面板（含停靠窗口的 Begin/End） */
		void OnImGuiRender();

		/* 几何统计的数字显示（纯函数，headless 可断言）：万以下原样、再往上 12.3k、
		 * 百万以上 4.56M —— 值列只有一两百像素，全位数既放不下也没人会去数 */
		static const char* FormatCount(uint64_t value, char* out, size_t out_size);

		/* 几何统计卡的一行（纯数据；headless 断言显示语义用）。
		 * Tooltip 只给口径容易看错的几行填，空的就不弹 */
		struct GeometryRow
		{
			std::string Label;
			std::string Value;
			std::string Tooltip;
		};

		/* 几何统计卡的行内容（纯函数，headless 可断言）：未结算时七行都是占位；
		 * 草按通道拆三行 —— 同一份剔除结果会被颜色 / 速度 / 阴影各提交一遍 */
		static void BuildGeometryRows(const RenderStatisticsSnapshot& stats,
			std::vector<GeometryRow>& out_rows);

		/* 图表的数据类型与布局在共享组件里，面板只保留别名 */
		using TreemapBlock = TimingChart::TreemapBlock;
		using FlameBlock = TimingChart::FlameBlock;
		using GpuTreemapBlock = TimingChart::GpuTreemapBlock;
		using GpuFlameBlock = TimingChart::GpuFlameBlock;

		/* Treemap 布局（squarified；纯函数，headless 可断言）：把 root 的子树铺进
		 * width × height —— 每层 =「自身 self 块 + 子节点块」（权重 = 各自动耗时），
		 * 面积与耗时成正比、父块包含子块、父块先序输出。inset = 每层向内留边。 */
		static void LayoutTreemap(const CPUTimeScopeNode& root, float width, float height,
			float inset, std::vector<TreemapBlock>& out_blocks);
		static void LayoutTreemap(const GPUTimeScopeNode& root, float width, float height,
			float inset, std::vector<GpuTreemapBlock>& out_blocks);

		/* 火焰图布局（嵌套；纯函数，headless 可断言）：块 = 作用域聚合 —— 横向 = 帧内顺序
		 *（CPU 按起始时间排、GPU 按提交顺序），纵向 = 调用深度（帧根在最上，每深度一行）。
		 * 折叠命中的节点「本块保留、不再下钻」；显示根不折。
		 * root_path = 显示根的绝对 label 链（收起键的前缀基准）。 */
		static void LayoutFlame(const CPUTimeScopeNode& root,
			const std::unordered_set<std::string>* collapsed, const std::string& root_path,
			std::vector<FlameBlock>& out_blocks);
		static void LayoutFlame(const GPUTimeScopeNode& root,
			const std::unordered_set<std::string>* collapsed, const std::string& root_path,
			std::vector<GpuFlameBlock>& out_blocks);

		/* ---- 计时树搜索（CPU / GPU 卡共用的纯函数，headless 可断言）----
		 * filter 须已小写化；空串 = 全部命中。NodeMatches = 自身标签命中（大小写无关子串）；
		 * SubtreeMatches = 自身或任一后代命中（行的可见性）；MatchCount = 命中数（含根）。 */
		template <typename NodeT>
		static bool TimingNodeMatches(const NodeT& node, const std::string& filter)
		{
			return ContainsCaseInsensitive(node.Label.c_str(), filter);
		}

		template <typename NodeT>
		static bool TimingSubtreeMatches(const NodeT& node, const std::string& filter)
		{
			if (TimingNodeMatches(node, filter))
				return true;

			for (const auto& child : node.Children)
			{
				if (TimingSubtreeMatches(child, filter))
					return true;
			}
			return false;
		}

		template <typename NodeT>
		static uint32_t TimingMatchCount(const NodeT& node, const std::string& filter)
		{
			uint32_t count = TimingNodeMatches(node, filter) ? 1u : 0u;
			for (const auto& child : node.Children)
				count += TimingMatchCount(child, filter);
			return count;
		}

		/* 计时树搜索（自检 / 外部驱动用）：写输入缓冲并即时生效；空串 = 清空过滤。
		 * 命中计数在树表绘制时结算（非树视图 / 无数据时保持旧值）。 */
		void SetCPUTimingSearchFilter(const std::string& filter);
		void SetGPUTimingSearchFilter(const std::string& filter);
		[[nodiscard]] const std::string& GetCPUTimingSearchFilter() const { return m_CpuView.Filter; }
		[[nodiscard]] const std::string& GetGPUTimingSearchFilter() const { return m_GpuView.Filter; }
		[[nodiscard]] uint32_t GetCPUTimingSearchMatchCount() const { return m_CpuView.MatchCount; }
		[[nodiscard]] uint32_t GetGPUTimingSearchMatchCount() const { return m_GpuView.MatchCount; }

		/* 追踪文本开关（录制期同步写盘的偏好；自检 / 外部驱动用）：未录制时只记偏好，
		 * 实际写盘会话与录制同起同停（SyncCPUTraceTextSession） */
		void SetCPUTraceTextEnabled(bool enabled) { m_CpuTraceTextEnabled = enabled; }
		[[nodiscard]] bool IsCPUTraceTextEnabled() const { return m_CpuTraceTextEnabled; }

		/* 计时卡看哪棵树（自检 / 外部驱动用） */
		void SetTimingSource(TimingSource source) { m_TimingSource = source; }
		[[nodiscard]] TimingSource GetTimingSource() const { return m_TimingSource; }
		/* 时间轴当前落在哪一帧（自检用只读） */
		[[nodiscard]] uint32_t GetResolvedFrameId() const { return ResolveCaptureFrameId(); }

		/* 图表视图 / 下钻深度 / 折叠数 / 缩放（自检用只读；卡片与弹窗各自的视图页） */
		[[nodiscard]] TimingChartView GetCpuChartView() const { return m_CpuView.View; }
		[[nodiscard]] TimingChartView GetCpuChartViewWindow() const { return m_CpuView.WindowView; }
		[[nodiscard]] TimingChartView GetGpuChartView() const { return m_GpuView.View; }
		/* 冻结 / 解冻当前帧：抓帧按钮与自检共用 */
		void ToggleCaptureFrame(TimingSource source);

		/* 开关一路采集通道：下拉点击与自检共用 */
		void ToggleChannel(uint32_t index);
		[[nodiscard]] ProfileChannel GetChannelFilter() const { return m_ChannelFilter; }

		/* 最近一帧三格切换器的实际宽度：行尾按钮会挤它，自检按这个算点击位置 */
		[[nodiscard]] float GetSwitcherWidth() const { return m_SwitcherWidth; }

		/* 按标签路径设置 Treemap 下钻深度：点击命中、面包屑、键盘导航共用这条入口，
		 * 外部驱动（自检）也不依赖画布坐标。空路径 = 回根。 */
		void SetCpuTreemapZoom(const std::vector<std::string>& path);
		void SetGpuTreemapZoom(const std::vector<std::string>& path);

		[[nodiscard]] uint32_t GetCpuTreemapZoomDepth() const
		{
			return static_cast<uint32_t>(m_CpuView.TreemapZoom.size());
		}
		[[nodiscard]] uint32_t GetGpuTreemapZoomDepth() const
		{
			return static_cast<uint32_t>(m_GpuView.TreemapZoom.size());
		}
		[[nodiscard]] uint32_t GetCpuFlameCollapsedCount() const
		{
			return static_cast<uint32_t>(m_CpuView.FlameCollapsed.size());
		}
		[[nodiscard]] uint32_t GetGpuFlameCollapsedCount() const
		{
			return static_cast<uint32_t>(m_GpuView.FlameCollapsed.size());
		}
		[[nodiscard]] float GetCpuFlameZoom() const { return m_CpuView.FlameZoom; }
		[[nodiscard]] float GetCpuFlamePan() const { return m_CpuView.FlamePan; }

		/* 抓取当前帧（实时视图冻结中；自检用只读） */
		[[nodiscard]] bool IsCpuFrameFrozen() const { return m_CpuFrozen; }
		[[nodiscard]] bool IsGpuFrameFrozen() const { return m_GpuFrozen; }
		/* 抓取时刻的帧号（冻结期间弹窗浮标显示它，不再随实时跳动；自检用只读） */
		[[nodiscard]] uint32_t GetCpuFrozenFrameId() const { return m_CpuFrozenFrameId; }

		/* 图表弹窗：开着没 / 最大化没 / 跟着哪个源（自检用只读） */
		[[nodiscard]] bool IsChartWindowOpen() const { return m_ChartWindowOpen; }
		[[nodiscard]] bool IsChartWindowMaximized() const { return m_ChartWindowMaximized; }
		[[nodiscard]] TimingSource GetChartWindowSource() const { return m_ChartWindowSource; }

	private:
		void ShowFrameStatsCard();
		/* 计时卡：CPU / GPU 两条采集共用一个卡 —— 一套 Live/Record 模式、一条时间轴，
		 * 树按 TimingSource 切换 */
		void ShowTimingsCard();
		/* 计时区：按源取显示根，再交给同一份图表区（工具条 + 三种视图） */
		void ShowTimingArea();
		/* 图表区本体（一个源）：树表 / 矩形图 / 火焰图，CPU 与 GPU 只差在 cpp 里的 traits。
		 * in_window = 画在弹窗里（视图页与卡片独立、尺寸铺满窗口、条目附耗时） */
		template <typename NodeT>
		void ShowTimingAreaBody(TimingSource source, const NodeT& root, TimingViewState& state,
			bool in_window);
		/* 工具条（卡 / 弹窗、CPU / GPU 同款）：视图三格 + [帧号] + 搜索 + [抓取] + 行尾钮 */
		void ShowChartBar(TimingSource source, TimingViewState& state, bool in_window);
		/* 两棵树的树表视图（列与悬浮读数各按自己的字段） */
		void ShowTimingTree(const CPUTimeScopeNode& root);
		void ShowTimingTree(const GPUTimeScopeNode& root);
		/* 几何统计卡：本帧的提交次数与几何吞吐（Draw Call / 三角形 / 顶点 / 实例）。
		 * 数据源 = RenderStatistics，帧边界与 GPU 计时器一致 */
		void ShowGeometryStatsCard();
		/* 帧耗时曲线：三线（Frame / CPU / GPU）+ 悬停读值；需要至少两帧历史才画线 */
		void ShowFrameTimeGraph(float height);
		/* 计时工具栏：暂停 / 继续、上一帧 / 下一帧、峰值 / 谷值、清空、导出 CSV、
		 * 追踪文本 —— 一行等大图标按钮，只在录制视图出现（它管的是回放） */
		void ShowTimingsToolbar();
		void ShowChannelFilterPopup();
		/* 共用时间轴：CPU / GPU 两条曲线按帧号对齐，一条滑条同时驱动两棵树 */
		void ShowCaptureTimeline();
		/* 追踪文本写盘会话：开关打开且采集未暂停时在写 */
		void SyncCPUTraceTextSession();
		/* 当前回放帧号：未选中（0）或选中的帧已被环形窗口覆盖时，取最新一帧 */
		[[nodiscard]] uint32_t ResolveCaptureFrameId() const;
		/* 当前这棵树属于哪一帧（弹窗行的帧号浮标）：录制 = 选中的帧，冻结 = 抓取时刻，
		 * 实时 = 该源最新结果帧（GPU 结果晚两帧，不能用引擎当前帧号） */
		[[nodiscard]] uint32_t CurrentDisplayFrameId(TimingSource source) const;
		/* 显示根（卡与弹窗共用）：录制视图 = 选中录制帧（无效时返回空）；冻结 = 快照；
		 * 其余 = 实时树 */
		[[nodiscard]] const CPUTimeScopeNode* ResolveCpuDisplayRoot() const;
		[[nodiscard]] const GPUTimeScopeNode* ResolveGpuDisplayRoot() const;
		/* 图表弹窗（「在窗口中打开」弹出）：独立顶层窗口，跟着打开它的那个源 */
		void ShowTimingChartWindow();
		/* 弹窗最大化 / 还原切换（行尾按钮）：最大化贴满主视口工作区，还原回浮动矩形 */
		void ToggleChartWindowMaximized();
		/* 抓取当前帧切换（卡 / 弹窗共用）：冻结拍快照 + 记帧号；再点解冻 */

		/* 回放选中的帧号（0 = 未选中，跟随最新）；CPU / GPU 两棵树共用同一条时间轴 */
		uint32_t m_ReplayFrameId{ 0 };
		/* 计时卡看哪棵树（默认 CPU）；只有一设计时开着时自动跟随 */
		TimingSource m_TimingSource{ TimingSource::CPU };
		/* CPU / GPU 源页签的选中块位置（0 = CPU、1 = GPU） */
		float m_SourceSlide{ 0.0f };

		/* 计时卡的观测方式：false = 实时树 / true = 录制视图（滚动窗口回放） */
		bool m_RecordView{ false };
		/* 滑动页签的选中块位置（0 = 实时页签、1 = 录制页签）：指数趋近产生滑动动画 */
		float m_TabSlide{ 0.0f };

		/* 两个源的树表 / 图表状态：选中行、搜索、视图页、下钻、折叠、缩放 */
		TimingViewState m_CpuView{};
		TimingViewState m_GpuView{};

		/* 追踪文本（HELIOS_PROFILE）：录制期同步写盘的偏好 + 会话归属（谁开谁关 ——
		 * 只收尾本面板开的会话，别动外部手动开的） */
		bool m_CpuTraceTextEnabled{ false };
		bool m_CpuTraceTextSessionOwned{ false };

		/* 抓取当前帧（实时视图里、搜索右侧的相机钮）：冻结 = 显示根改用当前帧快照，
		 * 不再跟随实时数据刷新，便于逐行细看；再点解冻。录制视图下按钮不出现。
		 * 弹窗行也有同一个钮（共享同一冻结态）；帧号浮标显示抓取时刻的帧号。 */
		bool m_CpuFrozen{ false };
		CPUTimeScopeNode m_CpuFrozenRoot{};
		uint32_t m_CpuFrozenFrameId{ 0 };
		bool m_GpuFrozen{ false };
		GPUTimeScopeNode m_GpuFrozenRoot{};
		uint32_t m_GpuFrozenFrameId{ 0 };

		/* 图表弹窗：跟着打开它的源（标题随之变化）；最大化期间窗口自身存的尺寸每帧被覆盖，
		 * 还原矩形得自己留一份（头文件不引 imgui.h，矩形用裸 float 存） */
		bool m_ChartWindowOpen{ false };
		TimingSource m_ChartWindowSource{ TimingSource::CPU };
		bool m_ChartWindowMaximized{ false };
		bool m_ChartWindowRestorePending{ false };
		float m_ChartRestoreX{ 0.0f };
		float m_ChartRestoreY{ 0.0f };
		float m_ChartRestoreW{ 0.0f };
		float m_ChartRestoreH{ 0.0f };
	};
}
