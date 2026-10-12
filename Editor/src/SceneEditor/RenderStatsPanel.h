#pragma once
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>
#include "EditorCommon.h"
#include "Helios/Core/CPUTimerProfiler.h"
#include "Helios/Renderer/RenderQuery.h"

namespace Helios
{
	class GPUTimerRecorder;

	/* 「Render Stats」面板：每帧 CPU / GPU 性能统计。三张卡 —— Frame（FPS / 帧间隔 /
	 * CPU 阶段 / GPU 合计 + 三线历史曲线）、CPU Timings（函数级作用域：Live / 录制回放
	 * + 树表 / Treemap / 火焰图三种视图）和 GPU Timings（录制 / 逐帧回放 / 导出 CSV +
	 * 耗时树）。数据源 = FrameTimeProfiler + CPUTimerProfiler + RenderQueryProfiler。 */
	class RenderStatsPanel
	{
	public:
		/* CPU 卡的图表视图（公开供自检断言） */
		enum class CpuChartView : uint8_t
		{
			Tree = 0,  /* 树表：调用结构 + total / self */
			Treemap,   /* 函数热力：面积 = 耗时占比，点击下钻 */
			Flame,     /* 时间轴火焰图：块 = 帧内区间（横向 = 时间） */
			Count
		};
		/* 画整个面板（含停靠窗口的 Begin/End） */
		void OnImGuiRender();

		/* GPU 录制导出为 CSV 文本（宽表：一行一帧 —— 帧号 / 根节点总耗时 + 顶点 / 片元
		 * 细分 / 每个作用域节点两列：总耗时 + 顶点细分，片元 = 总 − 顶点；节点列名 =
		 * 标签路径，集合按帧取并集、缺列留空）。纯函数（不做任何 IO），headless 检查断言。 */
		static std::string BuildRecordingCsv(const GPUTimerRecorder& recorder);

		/* 导出落盘的路径收尾：系统保存框不保证补后缀（macOS 面板不会自动加）——名称未以
		 * ".csv"（大小写不敏感）结尾时补上，已有则不重复；空路径（取消）原样返回。 */
		static std::string EnsureRecordingExtension(const std::string& path);

		/* Treemap 布局块（纯数据；headless 检查断言用） */
		struct TreemapBlock
		{
			const ResultCPUTimerNode* Node{ nullptr };
			bool IsSelf{ false };       /* true = 节点自身代码段块（不可下钻） */
			float X{ 0.0f };
			float Y{ 0.0f };
			float Width{ 0.0f };
			float Height{ 0.0f };
			int32_t ParentBlock{ -1 };  /* 包含它的父块下标（-1 = 显示根） */
		};

		/* Treemap 布局（squarified；纯函数，headless 可断言）：把 root 的子树铺进
		 * width × height —— 每层 =「自身 self 块 + 子节点块」（权重 = 各自动耗时），
		 * 面积与耗时成正比、父块包含子块、父块先序输出。inset = 每层向内留边。 */
		static void LayoutTreemap(const ResultCPUTimerNode& root, float width, float height,
			float inset, std::vector<TreemapBlock>& out_blocks);

		/* 火焰图块（纯数据；headless 检查断言用）：X0/X1 = 根宽归一化（0..1）的左右边界；
		 * 子块按起始时间排、宽度按总耗时占比铺满父块（父子直接相连、收起时其余块位置不变）。
		 * Depth = 调用深度（一行一深度）；ParentBlock = 先序父块下标（-1 = 显示根）；
		 * Collapsed = 子树已收起（右端标「＋」，点击展开） */
		struct FlameBlock
		{
			const ResultCPUTimerNode* Node{ nullptr };
			uint32_t Depth{ 0 };
			double X0{ 0.0 };
			double X1{ 1.0 };
			int32_t ParentBlock{ -1 };
			bool Collapsed{ false };
		};

		/* 火焰图布局（时间序嵌套；纯函数，headless 可断言）：块 = 调用区间聚合 —— 横向 =
		 * 帧内时间序（子块按起始时间排、宽度按总耗时占比铺满父块），纵向 = 调用深度
		 *（帧根在最上，每深度一行）。折叠命中的节点「本块保留、不再下钻」；显示根不折。
		 * root_path = 显示根的绝对 label 链（收起键的前缀基准）。 */
		static void LayoutFlame(const ResultCPUTimerNode& root,
			const std::unordered_set<std::string>* collapsed, const std::string& root_path,
			std::vector<FlameBlock>& out_blocks);

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
		[[nodiscard]] const std::string& GetCPUTimingSearchFilter() const { return m_CpuTimingSearchFilter; }
		[[nodiscard]] const std::string& GetGPUTimingSearchFilter() const { return m_GpuTimingSearchFilter; }
		[[nodiscard]] uint32_t GetCPUTimingSearchMatchCount() const { return m_CpuTimingSearchMatchCount; }
		[[nodiscard]] uint32_t GetGPUTimingSearchMatchCount() const { return m_GpuTimingSearchMatchCount; }

		/* 追踪文本开关（录制期同步写盘的偏好；自检 / 外部驱动用）：未录制时只记偏好，
		 * 实际写盘会话与录制同起同停（SyncCPUTraceTextSession） */
		void SetCPUTraceTextEnabled(bool enabled) { m_CpuTraceTextEnabled = enabled; }
		[[nodiscard]] bool IsCPUTraceTextEnabled() const { return m_CpuTraceTextEnabled; }

		/* CPU 卡的当前图表视图 / Treemap 下钻深度 / 火焰图折叠数 / 放大窗口开关（自检用只读） */
		[[nodiscard]] CpuChartView GetCpuChartView() const { return m_CpuChartView; }
		/* 放大窗口的图表视图（与卡片独立 —— 弹窗切图不动卡片；自检用只读） */
		[[nodiscard]] CpuChartView GetCpuChartViewWindow() const { return m_CpuChartViewWindow; }
		/* 抓取当前帧（实时视图冻结中；自检用只读） */
		[[nodiscard]] bool IsCpuFrameFrozen() const { return m_CpuFrozen; }
		[[nodiscard]] bool IsGpuFrameFrozen() const { return m_GpuFrozen; }
		/* 抓取时刻的帧号（冻结期间弹窗浮标显示它，不再随实时跳动；自检用只读） */
		[[nodiscard]] uint32_t GetCpuFrozenFrameId() const { return m_CpuFrozenFrameId; }
		[[nodiscard]] uint32_t GetCpuTreemapZoomDepth() const { return static_cast<uint32_t>(m_CpuTreemapZoom.size()); }
		[[nodiscard]] uint32_t GetCpuFlameCollapsedCount() const { return static_cast<uint32_t>(m_CpuFlameCollapsed.size()); }
		/* 火焰图视图窗口（缩放 / 平移；自检用只读）：缩放 ≥ 1、平移 = 归一化左缘 */
		[[nodiscard]] float GetCpuFlameZoom() const { return m_CpuFlameZoom; }
		[[nodiscard]] float GetCpuFlamePan() const { return m_CpuFlamePan; }
		[[nodiscard]] bool IsCpuChartWindowOpen() const { return m_CpuChartWindowOpen; }
		[[nodiscard]] bool IsCpuChartWindowMaximized() const { return m_CpuChartWindowMaximized; }

	private:
		void ShowFrameStatsCard();
		void ShowCPUTimingsCard();
		void ShowGPUTimingsCard();
		/* 帧耗时曲线：三线（Frame / CPU / GPU）+ 悬停读值；需要至少两帧历史才画线 */
		void ShowFrameTimeGraph(float height);
		/* 录制工具栏：录制 / 停止、上一帧 / 下一帧、峰值 / 谷值、清空、导出 CSV ——
		 * 一行等大图标按钮（录制视图常显；实时视图只在录制进行中显示，避免录制状态失联） */
		void ShowGPUTimerToolbar();
		/* GPU 录制的逐帧回放：帧间曲线 + 选帧滑条 + 概要行；
		 * 有录制内容且不在录制中时显示，改动 m_ReplayFrame（-1 = 未选中） */
		void ShowGPUTimerRecordingUI();
		/* CPU 版工具栏 / 回放（对偶 GPU；本期不含导出，列 = 根总耗时）。
		 * 工具栏首钮 = 追踪文本开关（图标钮，沿用录制选项语义） */
		void ShowCPUTimerToolbar();
		void ShowCPUTimerRecordingUI();
		/* 追踪文本写盘会话与录制同起同停（首钮只记偏好 —— 未录制时不开写） */
		void SyncCPUTraceTextSession();
		/* CPU 卡的三种图表视图（同一份结果树数据源；树表 = 整行可选中 + 搜索过滤）；
		 * Treemap 的 show_duration = 条目标签附耗时行（弹窗用；卡片保持名字-only） */
		void ShowCPUTreeView(const ResultCPUTimerNode& root);
		void ShowCPUTreemapView(const ResultCPUTimerNode& root, float width, float height, bool show_duration);
		void ShowCPUFlameView(const ResultCPUTimerNode& root, float width, float height);
		/* 图表放大窗口（「在窗口中打开」弹出）：独立顶层窗口，数据源与视图与卡共享 */
		void ShowCPUChartWindow();
		/* 弹窗最大化 / 还原切换（行尾按钮）：最大化贴满主视口工作区，还原回浮动矩形 */
		void ToggleCPUChartWindowMaximized();
		/* 抓取当前帧切换（CPU 卡行 / 弹窗行共用）：冻结拍快照 + 记帧号；再点解冻 */
		void ToggleCPUCaptureFrame();
		/* 图表的显示根（卡与放大窗口共用）：录制视图 = 选中录制帧；其余 = 实时树 */
		[[nodiscard]] const ResultCPUTimerNode* ResolveDisplayRoot() const;

		/* 回放选中的录制帧（-1 = 未选中，落到实时树） */
		int m_ReplayFrame{ -1 };

		/* 计时树里选中的行（节点 id；-1 = 未选中）—— 选中 / 悬停都高亮整行 */
		int m_SelectedNode{ -1 };

		/* GPU Timings 卡的观测方式：false = 实时树 / true = 录制视图（录制 + 逐帧回放） */
		bool m_RecordView{ false };
		/* 滑动页签的选中块位置（0 = 实时页签、1 = 录制页签）：指数趋近产生滑动动画 */
		float m_TabSlide{ 0.0f };
		/* 上一帧是否在录制中（用于"录制刚结束 → 停在最后一帧"） */
		bool m_WasRecording{ false };

		/* CPU Timings 卡状态（与 GPU 卡各自独立） */
		int m_CpuReplayFrame{ -1 };
		int m_CpuSelectedNode{ -1 };
		bool m_CpuRecordView{ false };
		float m_CpuTabSlide{ 0.0f };
		bool m_CpuWasRecording{ false };
		/* 计时树搜索（CPU / GPU 卡各一）：输入缓冲 + 小写化过滤词（空 = 不过滤）；
		 * 命中计数在树表绘制时结算（见 GetCPUTimingSearchMatchCount） */
		char m_CpuTimingSearch[128] = {};
		std::string m_CpuTimingSearchFilter{};
		uint32_t m_CpuTimingSearchMatchCount{ 0 };
		char m_GpuTimingSearch[128] = {};
		std::string m_GpuTimingSearchFilter{};
		uint32_t m_GpuTimingSearchMatchCount{ 0 };
		/* 追踪文本（HELIOS_PROFILE）：录制期同步写盘的偏好 + 会话归属（谁开谁关 ——
		 * 只收尾本面板开的会话，别动外部手动开的） */
		bool m_CpuTraceTextEnabled{ false };
		bool m_CpuTraceTextSessionOwned{ false };

		/* CPU 卡的图表视图 + 选中块滑动位置（0..2，指数趋近） + Treemap 下钻路径
		 * （label 链；换帧 / 换数据源后解析失败段自动截断） + 放大窗口开关 */
		CpuChartView m_CpuChartView{ CpuChartView::Tree };
		float m_CpuChartSlide{ 0.0f };
		/* 放大窗口的图表视图：与卡片独立（两处各自滑动）；下钻 / 折叠 / 缩放等状态仍共享 */
		CpuChartView m_CpuChartViewWindow{ CpuChartView::Tree };
		float m_CpuChartSlideWindow{ 0.0f };
		std::vector<std::string> m_CpuTreemapZoom{};
		/* 火焰图折叠集合：节点绝对 label 链（显示根起 "a/b/c"）。树变化后失效项不匹配、
		 * 自然失效（无需清理） */
		std::unordered_set<std::string> m_CpuFlameCollapsed{};
		/* 抓取当前帧（两个卡的实时视图里、搜索右侧的相机钮）：冻结 = 显示根改用当前帧快照，
		 * 不再跟随实时数据刷新，便于逐行细看；再点解冻。录制视图下按钮不出现。
		 * 弹窗行也有同一个钮（共享同一冻结态）；暂停期间帧号显示 FrozenFrameId（抓取时刻
		 * 的帧号，不再随实时跳动）。 */
		bool m_CpuFrozen{ false };
		ResultCPUTimerNode m_CpuFrozenRoot{};
		uint32_t m_CpuFrozenFrameId{ 0 };
		bool m_GpuFrozen{ false };
		ResultGPUTimerNode m_GpuFrozenRoot{};

		/* 火焰图视图窗口：缩放（1..64）+ 归一化左缘；滚轮缩放（锚在光标）、拖拽平移、右键
		 * 复位（FrameGraph 同一套手感）；缩放后右下角浮出缩略图（可拖 / 点跳）。
		 * DraggedDistance 区分拖拽与单击（未过阈值松开 = 单击）；ScrollGrab = 按在缩略图上 */
		float m_CpuFlameZoom{ 1.0f };
		float m_CpuFlamePan{ 0.0f };
		float m_CpuFlameDraggedDistance{ 0.0f };
		bool m_CpuFlameScrollGrab{ false };
		bool m_CpuChartWindowOpen{ false };
		/* 弹窗最大化：true 时每帧把窗口压在主视口工作区（跟随编辑器窗口尺寸变化）；
		 * 还原用最大化前记下的浮动矩形 —— 最大化期间窗口自身存的尺寸已被覆盖，须自留一份
		 * （头文件不引 imgui.h，矩形用裸 float 存） */
		bool m_CpuChartWindowMaximized{ false };
		bool m_CpuChartWindowRestorePending{ false };
		float m_CpuChartRestoreX{ 0.0f };
		float m_CpuChartRestoreY{ 0.0f };
		float m_CpuChartRestoreW{ 0.0f };
		float m_CpuChartRestoreH{ 0.0f };
	};
}
