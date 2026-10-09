#include "Pch.h"
#include "RenderStatsPanel.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <cstdio>
#include <functional>
#include "EditorIcons.h"
#include "PanelChrome.h"
#include "PanelRegistry.h"
#include "Helios/Core/FrameTimeProfiler.h"
#include "Helios/ImGui/EditorTheme.h"
#include "Helios/ImGui/ImGuiExtensions.h"
#include "Helios/Renderer/RenderQuery.h"
#include "Helios/Renderer/Renderer.h"

namespace Helios
{
	namespace
	{
		/* 无数据的占位文本（纯 ASCII，任何字体都能画） */
		constexpr const char* kNoDataText = "--";

		/* 曲线配色的语义（与图例同源）：Frame = 强调、CPU = Z 轴蓝、GPU = Y 轴绿 */
		const ImVec4 kFrameLineColor = EditorTheme::Token::Accent;
		const ImVec4 kCpuLineColor = EditorTheme::Token::AxisZ;
		const ImVec4 kGpuLineColor = EditorTheme::Token::AxisY;

		/* 画一条可能中断的折线：有效点连续段各自成笔，断开处不连线；
		 * 单点段画一个圆点（序列只有一帧时不至于什么都看不见）。 */
		void DrawBrokenSeries(ImDrawList* draw_list, uint32_t count,
			const std::function<bool(uint32_t)>& valid_at,
			const std::function<ImVec2(uint32_t)>& point_at,
			ImU32 color, float thickness)
		{
			uint32_t run_points = 0;
			ImVec2 last_point{ 0.0f, 0.0f };

			for (uint32_t index = 0; index <= count; ++index)
			{
				const bool valid = index < count && valid_at(index);
				if (valid)
				{
					last_point = point_at(index);
					draw_list->PathLineTo(last_point);
					++run_points;
					continue;
				}

				if (run_points >= 2)
					draw_list->PathStroke(color, 0, thickness);
				else if (run_points == 1)
					draw_list->AddCircleFilled(last_point, thickness * 1.5f, color);

				run_points = 0;
			}
		}
	}

	void RenderStatsPanel::OnImGuiRender()
	{
		PROFILE_FUNCTION();

		ImGui::Begin(Panel::kStatInfo);
		{
			const PanelChrome::HeaderRow header = PanelChrome::BeginHeaderRow(Icons::Id::Stats);
			PanelChrome::DrawHeaderTitle(header, "Render Stats");
			PanelChrome::EndHeaderRow(header);

			ShowFrameStatsCard();
			ShowGPUTimingsCard();
		}
		ImGui::End();
	}

	/* 帧统计卡：FPS / 帧间隔 / CPU 分阶段 / GPU 合计 + 三线历史曲线 */
	void RenderStatsPanel::ShowFrameStatsCard()
	{
		PROFILE_FUNCTION();

		const PanelChrome::Card card = PanelChrome::BeginCard("Frame");

		if (card.Open)
		{
			const FrameTimeProfiler::Snapshot& snapshot = FrameTimeProfiler::Instance().GetSnapshot();

			char buffer[64];

			snprintf(buffer, sizeof(buffer), "%.1f",
				snapshot.FrameMs > 0.0f ? 1000.0f / snapshot.FrameMs : 0.0f);
			ImGuiExt::DrawCommonTextUI("FPS", snapshot.FrameMs > 0.0f ? buffer : kNoDataText);

			snprintf(buffer, sizeof(buffer), "%.2f ms", snapshot.FrameMs);
			ImGuiExt::DrawCommonTextUI("Frame", buffer);

			ShowFrameTimeGraph(46.0f);

			snprintf(buffer, sizeof(buffer), "%.2f ms", snapshot.CpuMs);
			ImGuiExt::DrawCommonTextUI("CPU", buffer);

			for (uint32_t stage = 0; stage < FrameTimeProfiler::STAGE_COUNT; ++stage)
			{
				snprintf(buffer, sizeof(buffer), "%.2f ms", snapshot.StageMs[stage]);
				ImGuiExt::DrawCommonTextUI(
					FrameTimeProfiler::GetStageName(static_cast<FrameTimeProfiler::Stage>(stage)), buffer);
			}

			if (snapshot.GpuValid)
				snprintf(buffer, sizeof(buffer), "%.2f ms", snapshot.GpuMs);
			ImGuiExt::DrawCommonTextUI("GPU", snapshot.GpuValid ? buffer : kNoDataText);
		}

		PanelChrome::EndCard(card);
	}

	/* 三线历史曲线（最近 kHistorySize 帧的原始值）+ 图例 + 悬停读值 */
	void RenderStatsPanel::ShowFrameTimeGraph(float height)
	{
		PROFILE_FUNCTION();

		const FrameTimeProfiler& profiler = FrameTimeProfiler::Instance();
		const uint32_t count = profiler.GetHistoryCount();

		const ImGuiStyle& style = ImGui::GetStyle();
		const float width = ImGui::GetContentRegionAvail().x;
		const ImVec2 frame_min = ImGui::GetCursorScreenPos();
		const ImVec2 frame_max(frame_min.x + width, frame_min.y + height);

		ImGui::InvisibleButton("##FrameTimeGraph", ImVec2(width, height));
		const bool hovered = ImGui::IsItemHovered();

		ImDrawList* draw_list = ImGui::GetWindowDrawList();

		/* 底：比卡身暗一档的画布 + 细边框，与属性行区分开（画在内容通道上，不嵌套通道） */
		draw_list->AddRectFilled(frame_min, frame_max,
			ImGui::GetColorU32(EditorTheme::Token::Neutral1), 3.0f);
		draw_list->AddRect(frame_min, frame_max,
			ImGui::GetColorU32(EditorTheme::Token::Border), 3.0f);

		constexpr float kPadX = 4.0f;
		constexpr float kPadY = 3.0f;
		const ImVec2 plot_min(frame_min.x + kPadX, frame_min.y + kPadY);
		const ImVec2 plot_max(frame_max.x - kPadX, frame_max.y - kPadY);
		const float plot_width = ImMax(plot_max.x - plot_min.x, 1.0f);
		const float plot_height = ImMax(plot_max.y - plot_min.y, 1.0f);

		/* 纵轴上限：窗口内最大值上浮一档（帧间隔是主量级） */
		float value_max = 0.0f;
		for (uint32_t index = 0; index < count; ++index)
		{
			const FrameTimeProfiler::Sample& sample = profiler.GetHistorySample(index);
			value_max = ImMax(value_max, sample.FrameMs);
			value_max = ImMax(value_max, sample.CpuMs);
			if (sample.GpuValid)
				value_max = ImMax(value_max, sample.GpuMs);
		}
		value_max = ImMax(value_max * 1.15f, 1.0f);

		/* 60 FPS 预算参考线（16.7ms；超出上限时不画） */
		constexpr float kSixtyFpsMs = 1000.0f / 60.0f;
		if (value_max >= kSixtyFpsMs)
		{
			const float line_y = plot_max.y - kSixtyFpsMs / value_max * plot_height;
			const ImU32 dash_color = ImGui::GetColorU32(EditorTheme::Token::Separator);
			for (float x = plot_min.x; x < plot_max.x; x += 11.0f)
				draw_list->AddLine(ImVec2(x, line_y), ImVec2(ImMin(x + 6.0f, plot_max.x), line_y), dash_color);
		}

		if (count >= 1)
		{
			const auto column_x = [&](uint32_t index)
				{
					return count >= 2
						? plot_min.x + static_cast<float>(index) / static_cast<float>(count - 1) * plot_width
						: plot_min.x + plot_width * 0.5f;
				};
			const auto value_y = [&](float value)
				{ return plot_max.y - ImMin(value, value_max) / value_max * plot_height; };

			DrawBrokenSeries(draw_list, count,
				[](uint32_t) { return true; },
				[&](uint32_t index) { return ImVec2(column_x(index), value_y(profiler.GetHistorySample(index).FrameMs)); },
				ImGui::GetColorU32(kFrameLineColor), 1.2f);

			DrawBrokenSeries(draw_list, count,
				[](uint32_t) { return true; },
				[&](uint32_t index) { return ImVec2(column_x(index), value_y(profiler.GetHistorySample(index).CpuMs)); },
				ImGui::GetColorU32(kCpuLineColor), 1.2f);

			DrawBrokenSeries(draw_list, count,
				[&](uint32_t index) { return profiler.GetHistorySample(index).GpuValid; },
				[&](uint32_t index) { return ImVec2(column_x(index), value_y(profiler.GetHistorySample(index).GpuMs)); },
				ImGui::GetColorU32(kGpuLineColor), 1.2f);

			/* 上限刻度：左上角小字（与图形的量级对齐） */
			char scale_text[32];
			snprintf(scale_text, sizeof(scale_text), "%.1f ms", value_max);
			draw_list->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.85f,
				ImVec2(plot_min.x + 2.0f, plot_min.y),
				ImGui::GetColorU32(EditorTheme::Token::TextDim), scale_text);
		}
		else
		{
			draw_list->AddText(ImVec2(plot_min.x + 2.0f, plot_min.y),
				ImGui::GetColorU32(EditorTheme::Token::TextDim), "Collecting...");
		}

		/* 悬停读值：按鼠标列找最近的历史帧 */
		if (hovered && count >= 1)
		{
			const float position = (ImGui::GetIO().MousePos.x - plot_min.x) / plot_width;
			const uint32_t index = static_cast<uint32_t>(ImClamp(
				position * static_cast<float>(count - 1), 0.0f, static_cast<float>(count - 1)));
			const FrameTimeProfiler::Sample& sample = profiler.GetHistorySample(index);

			ImGui::BeginTooltip();
			ImGui::Text("Frame  %.2f ms", sample.FrameMs);
			ImGui::Text("CPU    %.2f ms", sample.CpuMs);
			if (sample.GpuValid)
				ImGui::Text("GPU    %.2f ms", sample.GpuMs);
			else
				ImGui::TextDisabled("GPU    %s", kNoDataText);
			ImGui::EndTooltip();
		}

		/* 图例（三线颜色说明） */
		const auto legend_item = [&](const char* label, const ImVec4& color, bool first)
			{
				if (!first)
					ImGui::SameLine(0.0f, style.ItemSpacing.x * 2.0f);

				constexpr float kDotRadius = 3.0f;
				ImGui::Dummy(ImVec2(kDotRadius * 2.0f, ImGui::GetTextLineHeight()));
				const ImVec2 min = ImGui::GetItemRectMin();
				const ImVec2 max = ImGui::GetItemRectMax();
				draw_list->AddCircleFilled(
					ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f),
					kDotRadius, ImGui::GetColorU32(color));

				ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
				ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
				ImGui::TextUnformatted(label);
				ImGui::PopStyleColor();
			};

		legend_item("Frame", kFrameLineColor, true);
		legend_item("CPU", kCpuLineColor, false);
		legend_item("GPU", kGpuLineColor, false);
	}

	/* GPU 计时卡：开关 + 逐层耗时表（结果树；无数据节点以占位显示） */
	void RenderStatsPanel::ShowGPUTimingsCard()
	{
		PROFILE_FUNCTION();

		const PanelChrome::Card card = PanelChrome::BeginCard("GPU Timings", Icons::Id::Stats);

		if (card.Open)
		{
			/* GPU 计时器的开关：面板级开关，占卡身的一行 */
			bool timer_enabled = RenderQueryProfiler::Instance().IsEnabled();
			if (ImGuiExt::DrawCheckboxUI("GPU Timer", timer_enabled))
				RenderQueryProfiler::Instance().SetEnabled(timer_enabled);

			const ResultGPUTimerNode& timer_root = Renderer::GetGPUTimerRoot();
			if (timer_root.Label.empty())
			{
				ImGuiExt::DrawCommonTextUI("Timers", timer_enabled ? "No data" : "Off");
			}
			else
			{
				std::function<void(const ResultGPUTimerNode&)> show_node_recursively;
				show_node_recursively = [&show_node_recursively](const ResultGPUTimerNode& timer_node)
					{
						if (timer_node.Label.empty())
							return;

						ImGui::TableNextRow();
						ImGui::TableNextColumn();

						char label[64] = {};
						snprintf(label, sizeof(label), "(%d) %s", timer_node.QueryIndex, timer_node.Label.c_str());
						label[sizeof(label) - 1] = 0;

						const bool has_children = !timer_node.Children.empty();
						ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_DefaultOpen;
						if (!has_children)
							flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_Bullet;
						else
							flags |= ImGuiTreeNodeFlags_OpenOnArrow;

						const bool is_opened = ImGui::TreeNodeEx(
							reinterpret_cast<void*>(static_cast<intptr_t>(timer_node.QueryIndex)), flags, "%s", label);

						ImGui::TableNextColumn();
						if (timer_node.HasData)
							ImGui::Text("%.2f", timer_node.GPUTime);
						else
							ImGui::TextDisabled("%s", kNoDataText);

						/* 悬停显示细分：顶点 / 片元阶段各自时长（与抓帧对照时
						 * 分清"顶点瓶颈"还是"片元 / 带宽开销"；其余后端无细分显示 0） */
						if (timer_node.HasData && ImGui::IsItemHovered())
						{
							ImGui::BeginTooltip();
							ImGui::Text("Vertex   %.2f us", timer_node.GPUTimeVertex);
							ImGui::Text("Fragment %.2f us", timer_node.GPUTimeFragment);
							ImGui::EndTooltip();
						}

						if (is_opened && has_children)
						{
							for (const auto& child : timer_node.Children)
								show_node_recursively(child);

							ImGui::TreePop();
						}
					};

				/* 表头 + 行背景由主题配色；耗时列按"最长数字"定宽，避免列宽随帧抖动 */
				const ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg
					| ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoBordersInBody
					| ImGuiTableFlags_SizingFixedFit;

				if (ImGui::BeginTable("GPUTimings", 2, table_flags))
				{
					ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupColumn("us", ImGuiTableColumnFlags_WidthFixed,
						ImGui::CalcTextSize("00000.00").x);

					ImGui::TableHeadersRow();
					show_node_recursively(timer_root);
					ImGui::EndTable();
				}
			}
		}

		PanelChrome::EndCard(card);
	}
}
