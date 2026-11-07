#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "imgui.h"
#include "Helios/Profiler/Profiler.h"

namespace Helios
{
	/* 计时图表的纯数据与取数口子：Treemap / 火焰图共用，CPU 与 GPU 两棵树靠 traits 适配 */
	namespace TimingChart
	{
		/* 计时图的三种视图：树表 / 矩形热力 / 时间轴火焰（CPU 与 GPU 同款） */
		enum class ChartView : uint8_t
		{
			Tree = 0,
			Treemap,
			Flame,
			Count
		};

		/* 一个源的全部图表状态：视图页 + 搜索 + 选中行 + 下钻 / 折叠 / 缩放。
		 * CPU / GPU 各一份 —— 三种视图只认它，不按源分叉。 */
		struct ViewState
		{
			ChartView View{ ChartView::Tree };
			float Slide{ 0.0f };
			/* 弹窗的视图页与卡片独立（两处各滑各的），下钻 / 折叠 / 缩放仍共享 */
			ChartView WindowView{ ChartView::Tree };
			float WindowSlide{ 0.0f };
			/* 搜索：输入缓冲 + 小写化过滤词（空 = 不过滤）+ 命中计数（树表绘制时结算） */
			char Search[128] = {};
			std::string Filter{};
			uint32_t MatchCount{ 0 };
			/* 树表选中行（遍历序号；-1 = 未选中） */
			int SelectedNode{ -1 };
			/* Treemap 下钻路径（label 链；换帧后解析失败段自动截断） */
			std::vector<std::string> TreemapZoom{};
			/* 火焰图折叠集合（显示根起的绝对 label 链）+ 视图窗口（缩放 / 归一化左缘） */
			std::unordered_set<std::string> FlameCollapsed{};
			float FlameZoom{ 1.0f };
			float FlamePan{ 0.0f };
			float FlameDraggedDistance{ 0.0f };
			bool FlameScrollGrab{ false };
		};

	/* Treemap 布局块（纯数据；headless 检查断言用） */
	template <typename NodeT>
	struct TimingTreemapBlock
	{
		const NodeT* Node{ nullptr };
		bool IsSelf{ false };       /* true = 节点自身代码段块（不可下钻） */
		float X{ 0.0f };
		float Y{ 0.0f };
		float Width{ 0.0f };
		float Height{ 0.0f };
		int32_t ParentBlock{ -1 };  /* 包含它的父块下标（-1 = 显示根） */
	};

	/* 火焰图块（纯数据；headless 检查断言用）：X0/X1 = 根宽归一化（0..1）的左右边界；
	 * Depth = 调用深度（一行一深度）；ParentBlock = 先序父块下标（-1 = 显示根）；
	 * Collapsed = 子树已收起（右端标「＋」，点击展开） */
	template <typename NodeT>
	struct TimingFlameBlock
	{
		const NodeT* Node{ nullptr };
		uint32_t Depth{ 0 };
		double X0{ 0.0 };
		double X1{ 1.0 };
		int32_t ParentBlock{ -1 };
		bool Collapsed{ false };
	};

	using TreemapBlock = TimingTreemapBlock<CPUTimeScopeNode>;
	using FlameBlock = TimingFlameBlock<CPUTimeScopeNode>;
	using GpuTreemapBlock = TimingTreemapBlock<GPUTimeScopeNode>;
	using GpuFlameBlock = TimingFlameBlock<GPUTimeScopeNode>;

	inline void TimingLocationText(const CPUTimeScopeNode& node)
	{
		if (node.File == nullptr || node.Line == 0)
			return;

		const char* base = node.File;
		for (const char* cursor = node.File; *cursor != '\0'; ++cursor)
		{
			if (*cursor == '/' || *cursor == '\\')
				base = cursor + 1;
		}
		ImGui::Text("%s:%u", base, node.Line);
	}

	template <typename NodeT>
	struct TimingNodeTraits;

	template <>
	struct TimingNodeTraits<CPUTimeScopeNode>
	{
		static const char* Label(const CPUTimeScopeNode& node) { return node.Label.c_str(); }
		static double Total(const CPUTimeScopeNode& node) { return node.TotalUs; }
		static double Self(const CPUTimeScopeNode& node) { return node.SelfUs; }
		/* 火焰图横向顺序：子块按起始时间排 */
		static constexpr bool TimeOrdered = true;
		static double Begin(const CPUTimeScopeNode& node) { return node.BeginUs; }

		/* 悬浮读数的源相关行（树表 / 矩形图 / 火焰图共用） */
		static void DrawHoverExtra(const CPUTimeScopeNode& node, double share)
		{
			TimingLocationText(node);
			ImGui::Text("%.1f%% of view · %u calls", share * 100.0, node.CallCount);
			ImGui::Text("span %.1f - %.1f us", node.BeginUs, node.EndUs);
		}
	};

	template <>
	struct TimingNodeTraits<GPUTimeScopeNode>
	{
		static const char* Label(const GPUTimeScopeNode& node) { return node.Label.c_str(); }
		static double Total(const GPUTimeScopeNode& node) { return node.TotalUs; }
		/* 自身 = 总耗时扣掉子项：通道区间里没有单独的 self */
		static double Self(const GPUTimeScopeNode& node)
		{
			double children = 0.0;
			for (const auto& child : node.Children)
			{
				if (!child.Label.empty())
					children += child.TotalUs;
			}
			return node.TotalUs > children ? node.TotalUs - children : 0.0;
		}
		/* 没有起始时间：按提交顺序铺，不排序 */
		static constexpr bool TimeOrdered = false;
		static double Begin(const GPUTimeScopeNode&) { return 0.0; }

		static void DrawHoverExtra(const GPUTimeScopeNode& node, double share)
		{
			ImGui::Text("%.1f%% of view", share * 100.0);
			if (node.HasStageSplit)
			{
				/* 阶段各取并集，三段之间会交叠，加起来可能大于本行时长 */
				ImGui::Text("vertex   %.2f us", node.StageVertexUs);
				ImGui::Text("fragment %.2f us", node.StageFragmentUs);
				if (node.StageComputeUs > 0.0)
					ImGui::Text("compute  %.2f us", node.StageComputeUs);
			}
			else if (node.QueryIndex != INVALID_QUERY_NODE_INDEX)
			{
				ImGui::TextDisabled("query #%d", static_cast<int>(node.QueryIndex));
			}
		}
	};

	/* Treemap 布局（squarified；CPU / GPU 同款）：每层 =「自身 self 块 + 子节点块」，

		/* Treemap / 火焰图布局（纯函数，headless 可断言） */
		void LayoutTreemap(const CPUTimeScopeNode& root, float width, float height,
			float inset, std::vector<TreemapBlock>& out_blocks);
		void LayoutTreemap(const GPUTimeScopeNode& root, float width, float height,
			float inset, std::vector<GpuTreemapBlock>& out_blocks);
		void LayoutFlame(const CPUTimeScopeNode& root,
			const std::unordered_set<std::string>* collapsed, const std::string& root_path,
			std::vector<FlameBlock>& out_blocks);
		void LayoutFlame(const GPUTimeScopeNode& root,
			const std::unordered_set<std::string>* collapsed, const std::string& root_path,
			std::vector<GpuFlameBlock>& out_blocks);

		/* 绘制：Treemap（面积热力 + 下钻）/ 火焰图（时间轴 + 折叠缩放） */
		void DrawTreemap(const CPUTimeScopeNode& root, ViewState& state, float width, float height,
			bool show_duration);
		void DrawTreemap(const GPUTimeScopeNode& root, ViewState& state, float width, float height,
			bool show_duration);
		void DrawFlame(const CPUTimeScopeNode& root, ViewState& state, float width, float height);
		void DrawFlame(const GPUTimeScopeNode& root, ViewState& state, float width, float height);
	}
}
