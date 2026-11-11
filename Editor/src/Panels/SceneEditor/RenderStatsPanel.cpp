#include "Pch.h"
#include "RenderStatsPanel.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <unordered_map>
#include <vector>
#include "EditorUI/EditorIcons.h"
#include "EditorUI/PanelChrome.h"
#include "EditorUI/PanelRegistry.h"
#include "Panels/PanelWindow.h"
#include "Common/FileDialog.h"
#include "Helios/Common/PathUtils.h"
#include "Helios/Core/Logger.h"
#include "EditorUI/EditorTheme.h"
#include "Common/EditorCommon.h"
#include "EditorUI/ImGuiExtensions.h"
#include "EditorUI/Widgets/FrameSlider.h"
#include "Helios/Renderer/Renderer.h"
#include "Helios/Profiler/ProfilerExport.h"

namespace Helios
{
	namespace
	{
		/* 无数据的占位文本（纯 ASCII，任何字体都能画） */
		constexpr const char* kNoDataText = "--";

		/* 「计时树」的一行（CPU / GPU 卡共用）：整行一个 Selectable 管命中 / 悬停 / 选中（连
		 * 数值列一起），树节点叠回行首照旧画箭头与折叠。底色两边都不自绘 —— 整行行底统一
		 * 走 TableSetBgColor 的行背景通道（行高自动结算、列怎么分都盖满）。返回行状态。
		 * force_open：搜索过滤命中路径上的祖先行 —— 每帧强制展开（覆盖折叠状态）。 */
		struct TimingRowState
		{
			bool Open = false;
			bool Clicked = false;
		};

		TimingRowState TimingTreeRow(const void* tree_id, const char* label, bool has_children,
			bool default_open, bool selected, bool force_open = false)
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();

			const ImVec2 row_min = ImGui::GetCursorScreenPos();

			/* Selectable 只为拿"整行"的命中与悬停，底色全推透明（行底由下面自选颜色画） */
			ImGui::PushStyleColor(ImGuiCol_Header, EditorTheme::Token::Clear);
			ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorTheme::Token::Clear);
			ImGui::PushStyleColor(ImGuiCol_HeaderActive, EditorTheme::Token::Clear);
			ImGui::PushID(tree_id);
			const bool row_pressed = ImGui::Selectable("##row", selected,
				ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap);
			const bool row_hovered = ImGui::IsItemHovered();
			ImGui::PopID();

			/* 叠回行首再画树节点：表格的行缩进就在行首坐标里，位置与直接提交它一致 */
			ImGui::SetCursorScreenPos(row_min);

			/* 搜索过滤：命中路径上的祖先每帧强制展开 —— 命中恒可见 */
			if (force_open && has_children)
				ImGui::SetNextItemOpen(true, ImGuiCond_Always);

			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanFullWidth;
			if (default_open)
				flags |= ImGuiTreeNodeFlags_DefaultOpen;
			if (!has_children)
				flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_Bullet;
			else
				flags |= ImGuiTreeNodeFlags_OpenOnArrow;

			const bool is_opened = ImGui::TreeNodeEx(tree_id, flags, "%s", label);
			const bool tree_clicked = ImGui::IsItemClicked();
			const bool tree_hovered = ImGui::IsItemHovered();

			ImGui::PopStyleColor(3);

			/* 行底：选中 = 强调色 40%；悬停 = 白 12%；选中被悬停仍保住金底（RowHoverSelected，
			 * 与层级 / 资源面板同一笔账）。两件套的悬停状态可能互让，取并集再画。 */
			const bool hovered = row_hovered || tree_hovered;
			if (selected || hovered)
			{
				const ImU32 row_bg = selected
					? (hovered ? ImGui::GetColorU32(EditorTheme::RowHoverSelected)
						: ImGui::GetColorU32(ImGuiCol_Header))
					: ImGui::GetColorU32(ImGuiCol_HeaderHovered);
				ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, row_bg);
			}

			return { is_opened, row_pressed || tree_clicked };
		}

		/* 行内搜索的控制宽度：吃「行宽 − 切换器 − 行尾按钮 − 与切换器两档间距 − 行尾一档
		 * 间距」的余量（70..320；宽窗不过分拉长，余量在切换器与搜索之间留白）——
		 * 与切换器的间距用两档：两个圆角容器贴太近看着像重叠 */
		float TimingSearchWidth(float available, float switcher_width, float button_size)
		{
			return ImClamp(available - switcher_width - button_size - ImGui::GetStyle().ItemSpacing.x * 3.0f,
				70.0f, 320.0f);
		}

		/* 行内搜索输入（卡 / 窗的视图切换行，搜框右对齐）：输入即更新过滤词
		 *（空 = 不过滤）；悬浮显示命中计数（计数在树表绘制时结算，显示的是上一帧值）。 */
		void TimingSearchInput(const char* id, char* buffer, size_t buffer_size,
			std::string& filter, uint32_t match_count, float width)
		{
			Icons::BeginSearchInput();
			ImGui::SetNextItemWidth(width);
			if (ImGui::InputTextWithHint(id, "Search scopes...", buffer, buffer_size))
				filter = ToLowercase(buffer);
			Icons::EndSearchInput();
			if (ImGui::IsItemHovered() && !filter.empty())
				ImGui::SetTooltip("%u matched", match_count);
		}

		/* ---- CPU 图表（Treemap / 火焰图）公共件 ---- */


	}

	/* 大数字缩写（12.3k / 4.56M）：值列只有一两百像素，全位数既放不下也没人会去数 */
	const char* RenderStatsPanel::FormatCount(uint64_t value, char* out, size_t out_size)
	{
		if (value >= 1000000000ull)
			std::snprintf(out, out_size, "%.2fG", static_cast<double>(value) / 1000000000.0);
		else if (value >= 1000000ull)
			std::snprintf(out, out_size, "%.2fM", static_cast<double>(value) / 1000000.0);
		else if (value >= 10000ull)
			std::snprintf(out, out_size, "%.1fk", static_cast<double>(value) / 1000.0);
		else
			std::snprintf(out, out_size, "%llu", static_cast<unsigned long long>(value));
		return out;
	}

	/* 几何统计卡的行内容（纯函数）：未结算时都是占位；间接绘制并进 Draw Calls 行 ——
	 * 它的元素数与实例数在 GPU 参数块里，混进顶点 / 三角形会让这两行的口径失真。
	 * 草的提交按通道拆三行：颜色、阴影、速度各算各的，别拿总数猜是哪一趟。 */
	void RenderStatsPanel::BuildGeometryRows(const RenderStatisticsSnapshot& stats,
		std::vector<GeometryRow>& out_rows)
	{
		out_rows.clear();

		if (!stats.Valid)
		{
			out_rows.push_back({ "Draw Calls", kNoDataText, {} });
			out_rows.push_back({ "Grass Visible", kNoDataText, {} });
			out_rows.push_back({ "Grass Shadow", kNoDataText, {} });
			out_rows.push_back({ "Grass Velocity", kNoDataText, {} });
			out_rows.push_back({ "Triangles", kNoDataText, {} });
			out_rows.push_back({ "Vertices", kNoDataText, {} });
			out_rows.push_back({ "Instances", kNoDataText, {} });
			out_rows.push_back({ "UI Draw Calls", kNoDataText, {} });
			out_rows.push_back({ "UI CPU", kNoDataText, {} });
			out_rows.push_back({ "UI Atlas", kNoDataText, {} });
			return;
		}

		char count_text[24];
		FormatCount(stats.DrawCalls, count_text, sizeof(count_text));
		char draw_calls_text[48];
		if (stats.IndirectDrawCalls > 0)
		{
			std::snprintf(draw_calls_text, sizeof(draw_calls_text), "%s (%llu indirect)", count_text,
				static_cast<unsigned long long>(stats.IndirectDrawCalls));
		}
		else
		{
			std::snprintf(draw_calls_text, sizeof(draw_calls_text), "%s", count_text);
		}
		out_rows.push_back({ "Draw Calls", draw_calls_text,
			"Indirect calls include grass and its shadows. Their element counts sit in the GPU args, "
			"so they stay out of the rows below" });

		char value_text[24];
		out_rows.push_back({ "Grass Visible", FormatCount(stats.GrassVisibleDrawCalls, value_text, sizeof(value_text)),
			"Blade submits in the color pass. The flat underlay is not counted" });
		out_rows.push_back({ "Grass Shadow", FormatCount(stats.GrassShadowDrawCalls, value_text, sizeof(value_text)),
			"One submit per cascade segment. Empty segments still count" });
		out_rows.push_back({ "Grass Velocity", FormatCount(stats.GrassVelocityDrawCalls, value_text, sizeof(value_text)),
			"Blade submits in the velocity pass. Only present while TAA is on" });

		out_rows.push_back({ "Triangles", FormatCount(stats.Triangles, value_text, sizeof(value_text)), {} });
		out_rows.push_back({ "Vertices", FormatCount(stats.Vertices, value_text, sizeof(value_text)), {} });
		out_rows.push_back({ "Instances", FormatCount(stats.Instances, value_text, sizeof(value_text)), {} });

		/* 运行时 UI：与上面几行不同源，它不走 Renderer::Submit。
		 * 布局与绘制分开记，闸门卡的是两段之和（200 格背包小于 2ms） */
		char ui_draw_text[48];
		std::snprintf(ui_draw_text, sizeof(ui_draw_text), "%llu (%s verts)",
			static_cast<unsigned long long>(stats.UIDrawCalls),
			FormatCount(stats.UIVertices, value_text, sizeof(value_text)));
		out_rows.push_back({ "UI Draw Calls", ui_draw_text,
			"One call per RmlUi geometry batch. No cross-element merging yet" });

		char ui_cpu_text[48];
		std::snprintf(ui_cpu_text, sizeof(ui_cpu_text), "%.2f ms (layout %.2f)",
			static_cast<double>(stats.UIUpdateMs) + static_cast<double>(stats.UIRenderMs),
			static_cast<double>(stats.UIUpdateMs));
		out_rows.push_back({ "UI CPU", ui_cpu_text,
			"Context update plus render. Gate: a 200 slot backpack stays under 2 ms" });

		char atlas_text[32];
		if (stats.UIFontAtlasBytes >= 1024ull * 1024ull)
			std::snprintf(atlas_text, sizeof(atlas_text), "%.1f MB",
				static_cast<double>(stats.UIFontAtlasBytes) / (1024.0 * 1024.0));
		else
			std::snprintf(atlas_text, sizeof(atlas_text), "%.0f KB",
				static_cast<double>(stats.UIFontAtlasBytes) / 1024.0);
		out_rows.push_back({ "UI Atlas", atlas_text,
			"Generated font atlas pages. RGBA8, four times an R8 estimate" });
	}

	/* 面板自身的开销不计入采集树：这一段里把 Editor 通道关掉（其它子系统照常收）。
	 * 采集本身还在跑，关掉的只是面板这几个作用域。 */
	void RenderStatsPanel::OnImGuiRender()
	{
		/* 画自己时临时去掉 Editor 位，析构恢复成用户的选择（不是进入前的值） */
		const ScopedChannelMask channel_mask(m_ChannelFilter & ~ProfileChannel::Editor, m_ChannelFilter);
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Editor);

		/* 追踪文本与采集同起同停（状态驱动、每帧一次；面板收起 / 换视图也照常结算） */
		SyncCPUTraceTextSession();

		/* 共用时间轴与源页签都在计时卡里，这里只管画卡 */
		if (PanelWindow window(Panel::kStatInfo, &Panel::VisibleFlag(Panel::kStatInfo)); window)
		{
			ShowFrameStatsCard();
			ShowTimingsCard();
			ShowGeometryStatsCard();
		}

		/* 图表弹窗：独立顶层窗口，必须画在面板窗口之外 */
		ShowTimingChartWindow();
	}

	/* 帧统计卡：FPS / 帧间隔 / CPU 分阶段 / GPU 合计 + 三线历史曲线 */
	void RenderStatsPanel::ShowFrameStatsCard()
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Editor);

		const PanelChrome::Card card = PanelChrome::BeginCard("Frame");

		if (card.Open)
		{
			const FrameTimeSnapshot& snapshot = Profiler::Instance().GetFrameTime();

			char buffer[64];

			snprintf(buffer, sizeof(buffer), "%.1f",
				snapshot.FrameMs > 0.0f ? 1000.0f / snapshot.FrameMs : 0.0f);
			ImGuiExt::DrawCommonTextUI("FPS", snapshot.FrameMs > 0.0f ? buffer : kNoDataText);

			snprintf(buffer, sizeof(buffer), "%.2f ms", snapshot.FrameMs);
			ImGuiExt::DrawCommonTextUI("Frame", buffer);

			ShowFrameTimeGraph(Scaled(46.0f));

			snprintf(buffer, sizeof(buffer), "%.2f ms", snapshot.CPUMs);
			ImGuiExt::DrawCommonTextUI("CPU", buffer);

			for (uint32_t stage = 0; stage < Profiler::STAGE_COUNT; ++stage)
			{
				/* 本帧没采到这个阶段：保留上次的耗时但画占位，别让数字突然归零 */
				if (snapshot.StageValid[stage])
					snprintf(buffer, sizeof(buffer), "%.2f ms", snapshot.StageMs[stage]);
				else
					snprintf(buffer, sizeof(buffer), "--");
				ImGuiExt::DrawCommonTextUI(
					Profiler::GetStageName(static_cast<FrameStage>(stage)), buffer);
			}

			if (snapshot.GPUValid)
				snprintf(buffer, sizeof(buffer), "%.2f ms", snapshot.GPUMs);
			ImGuiExt::DrawCommonTextUI("GPU", snapshot.GPUValid ? buffer : kNoDataText);
		}

		PanelChrome::EndCard(card);
	}

	/* 几何统计卡：本帧提交了多少笔、多少几何。数据源 = RenderStatistics —— 累加在
	 * Renderer::SubmitDraw，帧边界与 GPU 计时器一致（Renderer::Update 结算上一帧）。
	 * 间接绘制（草地）的元素数与实例数在参数块里由 GPU 读，只计入 Draw Calls。 */
	void RenderStatsPanel::ShowGeometryStatsCard()
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Editor);

		const PanelChrome::Card card = PanelChrome::BeginCard("Geometry", Icons::Id::Cube);

		if (card.Open)
		{
			std::vector<GeometryRow> rows;
			BuildGeometryRows(Profiler::Instance().GetRenderStatistics(), rows);
			for (const GeometryRow& row : rows)
			{
				/* 整行当命中区：数字本身没交互，悬停看口径 */
				ImGui::BeginGroup();
				ImGuiExt::DrawCommonTextUI(row.Label, row.Value);
				ImGui::EndGroup();
				if (!row.Tooltip.empty() && ImGui::IsItemHovered())
					ImGui::SetTooltip("%s", row.Tooltip.c_str());
			}
		}

		PanelChrome::EndCard(card);
	}

	/* 三线历史曲线（最近 kHistorySize 帧的原始值）+ 图例 + 悬停读值 */
	void RenderStatsPanel::ShowFrameTimeGraph(float height)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Editor);

		const Profiler& profiler = Profiler::Instance();
		const uint32_t count = profiler.GetFrameHistoryCount();

		const ImGuiStyle& style = ImGui::GetStyle();
		const float width = ImGui::GetContentRegionAvail().x;
		const ImVec2 frame_min = ImGui::GetCursorScreenPos();
		const ImVec2 frame_max(frame_min.x + width, frame_min.y + height);

		ImGui::InvisibleButton("##FrameTimeGraph", ImVec2(width, height));
		const bool hovered = ImGui::IsItemHovered();

		ImDrawList* draw_list = ImGui::GetWindowDrawList();

		/* 底：比卡身暗一档的画布 + 细边框，与属性行区分开（画在内容通道上，不嵌套通道） */
		draw_list->AddRectFilled(frame_min, frame_max,
			ImGui::GetColorU32(EditorTheme::Token::Neutral1), Scaled(3.0f));
		draw_list->AddRect(frame_min, frame_max,
			ImGui::GetColorU32(EditorTheme::Token::Border), Scaled(3.0f));

		const float kPadX = Scaled(4.0f);
		const float kPadY = Scaled(3.0f);
		const ImVec2 plot_min(frame_min.x + kPadX, frame_min.y + kPadY);
		const ImVec2 plot_max(frame_max.x - kPadX, frame_max.y - kPadY);
		const float plot_width = ImMax(plot_max.x - plot_min.x, 1.0f);
		const float plot_height = ImMax(plot_max.y - plot_min.y, 1.0f);

		/* 纵轴上限：窗口内最大值上浮一档（帧间隔是主量级） */
		float value_max = 0.0f;
		for (uint32_t index = 0; index < count; ++index)
		{
			const FrameTimeSample& sample = profiler.GetFrameHistorySample(index);
			value_max = ImMax(value_max, sample.FrameMs);
			value_max = ImMax(value_max, sample.CPUMs);
			if (sample.GPUValid)
				value_max = ImMax(value_max, sample.GPUMs);
		}
		value_max = ImMax(value_max * 1.15f, 1.0f);

		/* 60 FPS 预算参考线（16.7ms；超出上限时不画） */
		constexpr float kSixtyFpsMs = 1000.0f / 60.0f;
		if (value_max >= kSixtyFpsMs)
		{
			const float line_y = plot_max.y - kSixtyFpsMs / value_max * plot_height;
			const ImU32 dash_color = ImGui::GetColorU32(EditorTheme::Token::Separator);
			for (float x = plot_min.x; x < plot_max.x; x += Scaled(11.0f))
				draw_list->AddLine(ImVec2(x, line_y), ImVec2(ImMin(x + Scaled(6.0f), plot_max.x), line_y), dash_color);
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

			::Helios::Widgets::DrawBrokenSeries(draw_list, count,
				[](uint32_t) { return true; },
				[&](uint32_t index) { return ImVec2(column_x(index), value_y(profiler.GetFrameHistorySample(index).FrameMs)); },
				ImGui::GetColorU32(::Helios::Widgets::kFrameLineColor), Scaled(1.2f));

			::Helios::Widgets::DrawBrokenSeries(draw_list, count,
				[](uint32_t) { return true; },
				[&](uint32_t index) { return ImVec2(column_x(index), value_y(profiler.GetFrameHistorySample(index).CPUMs)); },
				ImGui::GetColorU32(::Helios::Widgets::kCpuLineColor), Scaled(1.2f));

			::Helios::Widgets::DrawBrokenSeries(draw_list, count,
				[&](uint32_t index) { return profiler.GetFrameHistorySample(index).GPUValid; },
				[&](uint32_t index) { return ImVec2(column_x(index), value_y(profiler.GetFrameHistorySample(index).GPUMs)); },
				ImGui::GetColorU32(::Helios::Widgets::kGpuLineColor), Scaled(1.2f));

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
			const FrameTimeSample& sample = profiler.GetFrameHistorySample(index);

			ImGui::BeginTooltip();
			ImGui::Text("Frame  %.2f ms", sample.FrameMs);
			ImGui::Text("CPU    %.2f ms", sample.CPUMs);
			if (sample.GPUValid)
				ImGui::Text("GPU    %.2f ms", sample.GPUMs);
			else
				ImGui::TextDisabled("GPU    %s", kNoDataText);
			ImGui::EndTooltip();
		}

		/* 图例（三线颜色说明） */
		const auto legend_item = [&](const char* label, const ImVec4& color, bool first)
			{
				if (!first)
					ImGui::SameLine(0.0f, style.ItemSpacing.x * 2.0f);

				const float kDotRadius = Scaled(3.0f);
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

		legend_item("Frame", ::Helios::Widgets::kFrameLineColor, true);
		legend_item("CPU", ::Helios::Widgets::kCpuLineColor, false);
		legend_item("GPU", ::Helios::Widgets::kGpuLineColor, false);
	}

	/* 计时卡：CPU / GPU 两条采集共用一个卡 —— 一套 Live/Record 模式、一条时间轴，
	 * 树按源页签切换。两条录制都在后台常开，切页签只换看哪棵，不动采集。 */
	void RenderStatsPanel::ShowTimingsCard()
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Editor);

		const PanelChrome::Card card = PanelChrome::BeginCard("Timings", Icons::Id::Stats);

		if (card.Open)
		{
			ImGui::PushID("Timings");

			/* 两个采集开关并排：关掉只是停止采集，已缓冲的帧保留。
			 * 标签列按文本宽收紧 —— 用默认设计宽度会把复选框推到行中间，两个并排就散成两截 */
			const ImGuiStyle& card_style = ImGui::GetStyle();
			bool cpu_enabled = Profiler::Instance().IsCPUEnabled();
			bool gpu_enabled = Profiler::Instance().IsGPUEnabled();
			if (ImGuiExt::DrawCheckboxUI("CPU Timer", cpu_enabled,
				ImGui::CalcTextSize("CPU Timer").x + card_style.ItemInnerSpacing.x))
			{
				Profiler::Instance().SetCPUEnabled(cpu_enabled);
				if (!cpu_enabled)
					Profiler::Instance().SetCPURecording(false);
			}
			ImGui::SameLine(0.0f, card_style.ItemSpacing.x * 3.0f);
			if (ImGuiExt::DrawCheckboxUI("GPU Timer", gpu_enabled,
				ImGui::CalcTextSize("GPU Timer").x + card_style.ItemInnerSpacing.x))
			{
				Profiler::Instance().SetGPUEnabled(gpu_enabled);
				if (!gpu_enabled)
					Profiler::Instance().SetGPURecording(false);
			}

			const bool any_enabled = cpu_enabled || gpu_enabled;

			/* 一行两件事：左边观测方式（Live / Record），右边看哪棵树（CPU / GPU）。
			 * 两个都按内容取宽 —— 两条铺满整行的开关并列会显得一样重 */
			if (any_enabled)
			{
				const float row_width = ImGui::GetContentRegionAvail().x;
				const float mode_width = ImMin(row_width, ::Helios::Widgets::SlidingTabPairWidth("Live", "Record", true));
				const int switched = ::Helios::Widgets::SlidingTabPair("##TimingsViewTabs", "Live", Icons::Id::Live,
					"Record", Icons::Id::Record, m_RecordView, m_TabSlide, mode_width);
				if (switched == 0)
					m_RecordView = false;
				else if (switched == 1)
					m_RecordView = true;

				if (cpu_enabled && gpu_enabled)
				{
					const float source_width = ImMin(row_width - mode_width - card_style.ItemSpacing.x,
						::Helios::Widgets::SlidingTabPairWidth("CPU", "GPU", false));
					ImGuiExt::SameLineAt(row_width - source_width);
					const int source = ::Helios::Widgets::SlidingTabPair("##TimingSource", "CPU", Icons::Id::None,
						"GPU", Icons::Id::None, m_TimingSource == TimingSource::GPU, m_SourceSlide, source_width);
					if (source == 0)
						m_TimingSource = TimingSource::CPU;
					else if (source == 1)
						m_TimingSource = TimingSource::GPU;
				}
				else
				{
					m_TimingSource = cpu_enabled ? TimingSource::CPU : TimingSource::GPU;
					m_SourceSlide = m_TimingSource == TimingSource::GPU ? 1.0f : 0.0f;
				}
			}

			if (any_enabled && m_RecordView)
				ShowTimingsToolbar();

			const bool has_cpu = cpu_enabled && Profiler::Instance().GetCPUFrameCount() > 0;
			const bool has_gpu = gpu_enabled && Profiler::Instance().GetGPUFrameCount() > 0;
			const bool replaying = any_enabled && m_RecordView && (has_cpu || has_gpu);

			if (replaying)
				ShowCaptureTimeline();

			if (!any_enabled)
			{
				ImGuiExt::DrawCommonTextUI("Timers", "Off");
			}
			else if (m_RecordView && !replaying)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
				ImGui::TextWrapped("Capture is always running. The rolling window fills as soon as "
					"frames arrive; timings show up here right after.");
				ImGui::PopStyleColor();
			}
			else
			{
				/* CPU / GPU 同一套图表区：显示根按源取，视图与工具条共用 */
				ShowTimingArea();
			}

			ImGui::PopID();
		}

		PanelChrome::EndCard(card);
	}

	/* 图表区本体（一个源）：工具条 + 三种视图。两棵树的字段差异全在 traits 里，这里只分派。
	 * in_window = 画在弹窗里（视图页与卡片独立、尺寸铺满窗口、条目附耗时）。 */
	template <typename NodeT>
	void RenderStatsPanel::ShowTimingAreaBody(TimingSource source, const NodeT& root,
		TimingViewState& state, bool in_window)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Editor);

		if (root.Label.empty())
		{
			ImGuiExt::DrawCommonTextUI("Timers", "No data");
			return;
		}

		ShowChartBar(source, state, in_window);

		/* 弹窗开着（且看的就是这个源）：卡里的图表让位 —— 同一份数据只同时显示一处；
		 * 行留在原处，行尾按钮 / 弹窗标题栏 X 都能把视图收回来 */
		if (!in_window && m_ChartWindowOpen && m_ChartWindowSource == source)
		{
			ImGuiExt::DrawCommonTextUI("Timers", "Shown in the Timings chart window");
			return;
		}

		const float width = ImGui::GetContentRegionAvail().x;
		const float height = ImGui::GetContentRegionAvail().y;
		switch (in_window ? state.WindowView : state.View)
		{
		case TimingChartView::Treemap:
			::Helios::TimingChart::DrawTreemap(root, state, width, in_window ? height : 180.0f, in_window);
			break;
		case TimingChartView::Flame:
			::Helios::TimingChart::DrawFlame(root, state, width, in_window ? height : 150.0f);
			break;
		case TimingChartView::Tree:
		default:
			ShowTimingTree(root);
			break;
		}
	}

	/* 计时区：按源取显示根，再交给同一份图表区。GPU 结果异步解析，选中的帧可能还没数据 ——
	 * 单独给一句提示，别让人以为没在采集。 */
	void RenderStatsPanel::ShowTimingArea()
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Editor);

		if (m_TimingSource == TimingSource::GPU)
		{
			/* GPU 结果由异步查询解析，比 CPU 晚两帧左右 —— 选中的帧可能还没有数据 */
			const GPUTimeScopeNode* display_root = ResolveGpuDisplayRoot();
			if (display_root == nullptr)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
				ImGui::TextWrapped("No GPU results for this frame yet. Query results arrive a couple "
					"of frames late; step back a frame or wait a moment.");
				ImGui::PopStyleColor();
				return;
			}
			ShowTimingAreaBody(TimingSource::GPU, *display_root, m_GpuView, false);
			return;
		}

		const CPUTimeScopeNode* display_root = ResolveCpuDisplayRoot();
		if (display_root == nullptr)
		{
			ImGuiExt::DrawCommonTextUI("Timers", "No data");
			return;
		}
		ShowTimingAreaBody(TimingSource::CPU, *display_root, m_CpuView, false);
	}

	/* 计时图表工具条（卡 / 弹窗、CPU / GPU 同款）：左视图三格 + [帧号] + 右搜索 + [抓取] +
	 * 行尾钮（卡 = 在窗口中打开，弹窗 = 最大化 / 还原）。状态全走调用方给的那份 state。 */
	void RenderStatsPanel::ShowChartBar(TimingSource source, TimingViewState& state, bool in_window)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Editor);

		const ImGuiStyle& style = ImGui::GetStyle();
		const float row_width = ImGui::GetContentRegionAvail().x;
		const float tail_size = ImGui::GetFrameHeight();
		/* 抓取钮位：录制视图不出现（可抓的实时流只在实时视图里） */
		const float capture_size = tail_size;
		const float capture_reserve = !m_RecordView
			? capture_size + style.ItemSpacing.x : 0.0f;
		/* 通道筛选只管 CPU 采集，切到 GPU 源时不占位 */
		const float channel_reserve = source == TimingSource::CPU
			? tail_size + style.ItemSpacing.x : 0.0f;
		const float switcher_width = ImMin(::Helios::Widgets::TimingSwitcherWidth(),
			ImMax(row_width - 70.0f - tail_size - capture_reserve - channel_reserve
				- style.ItemSpacing.x * 3.0f, 0.0f));
		const float search_width = TimingSearchWidth(row_width, switcher_width,
			tail_size + capture_reserve + channel_reserve);

		TimingChartView& view = in_window ? state.WindowView : state.View;
		float& slide = in_window ? state.WindowSlide : state.Slide;
		const char* suffix = in_window ? "Window" : "";
		char switcher_id[32];
		char search_id[32];
		snprintf(switcher_id, sizeof(switcher_id), "##ChartTabs%s", suffix);
		snprintf(search_id, sizeof(search_id), "##TimingSearch%s", suffix);

		const int switched_view = ::Helios::Widgets::SlidingTabTriple(switcher_id,
			"Tree", Icons::Id::Menu, "Treemap", Icons::Id::Grid, "Flame", Icons::Id::Stats,
			static_cast<int>(view), slide, switcher_width);
		if (switched_view >= 0)
			view = static_cast<TimingChartView>(switched_view);
		m_SwitcherWidth = switcher_width;

		/* 弹窗行在切换条后补个帧号：回放 = 选中的帧；冻结 = 抓取时刻；实时 = 该源最新结果帧 */
		if (in_window)
		{
			ImGuiExt::SameLineAt(switcher_width + style.ItemSpacing.x * 2.0f);
			/* 与切换条的竖直中心对齐：SameLine 默认贴行顶、文字比图标矮会整体偏上 */
			ImGui::SetCursorPosY(ImGui::GetCursorPosY()
				+ (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
			ImGui::TextDisabled("Frame %u", CurrentDisplayFrameId(source));
		}

		ImGuiExt::SameLineAt(row_width - search_width - tail_size - capture_reserve - channel_reserve - style.ItemSpacing.x);
		TimingSearchInput(search_id, state.Search, IM_ARRAYSIZE(state.Search), state.Filter,
			state.MatchCount, search_width);

		/* 抓取当前帧（搜索右侧、实时视图）：冻结 = 显示根改用快照供逐行分析；再点解冻 */
		if (!m_RecordView)
		{
			ImGuiExt::SameLineAt(row_width - tail_size - capture_reserve);
			const bool frozen = source == TimingSource::CPU ? m_CpuFrozen : m_GpuFrozen;
			if (Icons::IconButton(Icons::Id::Capture, ImVec2(capture_size, capture_size), frozen,
				frozen ? "Resume live updates" : "Capture current frame (freeze for inspection)"))
				ToggleCaptureFrame(source);
		}

		/* 行尾：通道筛选（只影响 CPU 采集，GPU 源不显示）、抓帧、开窗 */
		if (channel_reserve > 0.0f)
		{
			ImGuiExt::SameLineAt(row_width - tail_size - capture_reserve - channel_reserve);
			if (Icons::IconButton(Icons::Id::Sliders, ImVec2(tail_size, tail_size),
				m_ChannelFilter != ProfileChannel::All, "Channels (CPU sampling only)"))
				ImGui::OpenPopup("##ChannelFilter");
			ShowChannelFilterPopup();
		}

		ImGuiExt::SameLineAt(row_width - tail_size);
		if (in_window)
		{
			if (Icons::IconButton(m_ChartWindowMaximized ? Icons::Id::Restore : Icons::Id::Maximize,
				ImVec2(tail_size, tail_size), m_ChartWindowMaximized,
				m_ChartWindowMaximized ? "Restore to floating window"
					: "Maximize (fill the editor window)"))
				ToggleChartWindowMaximized();
		}
		else if (Icons::IconButton(Icons::Id::FitView, ImVec2(tail_size, tail_size),
			m_ChartWindowOpen && m_ChartWindowSource == source, "Open in a window"))
		{
			const bool showing_this = m_ChartWindowOpen && m_ChartWindowSource == source;
			m_ChartWindowOpen = !showing_this;
			m_ChartWindowSource = source;
		}
	}

	/* CPU 卡的树表视图：函数级结果树（整行可选中；浅层默认展开） */
	void RenderStatsPanel::ShowTimingTree(const CPUTimeScopeNode& root)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Editor);

		/* 命中计数每帧随树结算（搜索框在工具条、悬浮提示用；自检可读） */
		m_CpuView.MatchCount = TimingMatchCount(root, m_CpuView.Filter);
		const bool filtering = !m_CpuView.Filter.empty();

		/* 行 id 用遍历序号（结构稳定时跨帧一致 —— 折叠状态可保持） */
		uint32_t node_id = 0;
		std::function<void(const CPUTimeScopeNode&, uint32_t)> show_node_recursively;
		show_node_recursively = [&show_node_recursively, &node_id, filtering, this](
			const CPUTimeScopeNode& timer_node, uint32_t depth)
			{
				if (timer_node.Label.empty())
					return;

				/* 搜索过滤：自身与后代都无命中的整支不画（过滤清空时恒可见） */
				if (filtering && !TimingSubtreeMatches(timer_node, m_CpuView.Filter))
					return;

				const uint32_t this_id = node_id++;
				const bool has_children = !timer_node.Children.empty();

				/* 只展开浅层，深层手动；点行任意一处（含箭头那一下）都选中这行；
				 * 过滤中：命中路径上的祖父行强制展开 */
				const TimingRowState row = TimingTreeRow(
					reinterpret_cast<void*>(static_cast<intptr_t>(this_id)),
					timer_node.Label.c_str(), has_children, depth < 2,
					m_CpuView.SelectedNode == static_cast<int>(this_id),
					filtering && !TimingNodeMatches(timer_node, m_CpuView.Filter));
				if (row.Clicked)
					m_CpuView.SelectedNode = static_cast<int>(this_id);

				ImGui::TableNextColumn();
				if (timer_node.HasData)
					ImGui::Text("%.2f", timer_node.TotalUs);
				else
					ImGui::TextDisabled("%s", kNoDataText);

				ImGui::TableNextColumn();
				if (timer_node.HasData)
					ImGui::Text("%.2f", timer_node.SelfUs);
				else
					ImGui::TextDisabled("%s", kNoDataText);

				/* 悬停显示补充分量：调用次数 + 首末区间（热力图 / 火焰图的同源数据） */
				if (timer_node.HasData && ImGui::IsItemHovered())
				{
					ImGui::BeginTooltip();
					::Helios::TimingChart::TimingLocationText(timer_node);
					ImGui::Text("%u calls/frame", timer_node.CallCount);
					ImGui::Text("span  %.1f - %.1f us", timer_node.BeginUs, timer_node.EndUs);
					ImGui::EndTooltip();
				}

				if (row.Open && has_children)
				{
					for (const auto& child : timer_node.Children)
						show_node_recursively(child, depth + 1);

					ImGui::TreePop();
				}
			};

		const ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg
			| ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoBordersInBody
			| ImGuiTableFlags_SizingFixedFit;

		if (ImGui::BeginTable("CPUTimings", 3, table_flags))
		{
			ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("total us", ImGuiTableColumnFlags_WidthFixed,
				ImGui::CalcTextSize("00000.00").x);
			ImGui::TableSetupColumn("self us", ImGuiTableColumnFlags_WidthFixed,
				ImGui::CalcTextSize("00000.00").x);

			ImGui::TableHeadersRow();
			show_node_recursively(root, 0);
			ImGui::EndTable();
		}
	}

	/* 布局入口（CPU / GPU 各一份重载，实现只有一份）：视图与 headless 检查共用 */
	void RenderStatsPanel::LayoutTreemap(const CPUTimeScopeNode& root, float width, float height,
		float inset, std::vector<TreemapBlock>& out_blocks)
	{
		::Helios::TimingChart::LayoutTreemap(root, width, height, inset, out_blocks);
	}

	void RenderStatsPanel::LayoutTreemap(const GPUTimeScopeNode& root, float width, float height,
		float inset, std::vector<GpuTreemapBlock>& out_blocks)
	{
		::Helios::TimingChart::LayoutTreemap(root, width, height, inset, out_blocks);
	}

	void RenderStatsPanel::LayoutFlame(const CPUTimeScopeNode& root,
		const std::unordered_set<std::string>* collapsed, const std::string& root_path,
		std::vector<FlameBlock>& out_blocks)
	{
		::Helios::TimingChart::LayoutFlame(root, collapsed, root_path, out_blocks);
	}

	void RenderStatsPanel::LayoutFlame(const GPUTimeScopeNode& root,
		const std::unordered_set<std::string>* collapsed, const std::string& root_path,
		std::vector<GpuFlameBlock>& out_blocks)
	{
		::Helios::TimingChart::LayoutFlame(root, collapsed, root_path, out_blocks);
	}

	/* 当前回放帧号：未选中（0）或选中的帧已不在环形窗口里时，落到"当前看的这份录制"的最新
	 * 一帧。必须跟着源走 —— GPU 结果比 CPU 晚两帧解析，用 CPU 的最新帧去查 GPU 永远查不到。 */
	uint32_t RenderStatsPanel::ResolveCaptureFrameId() const
	{
		const Profiler& profiler = Profiler::Instance();
		const bool cpu_ready = profiler.IsCPUEnabled() && profiler.GetCPUFrameCount() > 0;
		const bool gpu_ready = profiler.IsGPUEnabled() && profiler.GetGPUFrameCount() > 0;

		const auto from_gpu = [this, &profiler]()
			{
				if (m_ReplayFrameId != 0
					&& profiler.FindGPUFrame(m_ReplayFrameId) != INVALID_SCOPE_INDEX)
					return m_ReplayFrameId;
				return profiler.GetGPUFrameId(profiler.GetGPUFrameCount() - 1);
			};
		const auto from_cpu = [this, &profiler]()
			{
				if (m_ReplayFrameId != 0
					&& profiler.FindCPUFrame(m_ReplayFrameId) != INVALID_SCOPE_INDEX)
					return m_ReplayFrameId;
				return profiler.GetCPUFrameId(profiler.GetCPUFrameCount() - 1);
			};

		if (m_TimingSource == TimingSource::GPU && gpu_ready)
			return from_gpu();
		if (cpu_ready)
			return from_cpu();
		if (gpu_ready)
			return from_gpu();
		return 0;
	}

	/* 显示根（卡与弹窗共用）：录制视图 = 时间轴选中的帧（无效时机返回空）；冻结 = 快照；
	 * 其余 = 实时树（EMA） */
	const CPUTimeScopeNode* RenderStatsPanel::ResolveCpuDisplayRoot() const
	{
		if (!m_RecordView && m_CpuFrozen)
			return &m_CpuFrozenRoot;

		if (m_RecordView)
		{
			const uint32_t index = Profiler::Instance().FindCPUFrame(ResolveCaptureFrameId());
			return index == INVALID_SCOPE_INDEX ? nullptr : &Profiler::Instance().GetCPUFrame(index);
		}
		return &Profiler::Instance().GetCPURoot();
	}

	const GPUTimeScopeNode* RenderStatsPanel::ResolveGpuDisplayRoot() const
	{
		if (!m_RecordView && m_GpuFrozen)
			return &m_GpuFrozenRoot;

		if (m_RecordView)
		{
			const uint32_t index = Profiler::Instance().FindGPUFrame(ResolveCaptureFrameId());
			return index == INVALID_SCOPE_INDEX ? nullptr : &Profiler::Instance().GetGPUFrame(index);
		}
		return &Profiler::Instance().GetGPURoot();
	}

	/* 当前这棵树属于哪一帧（弹窗行的帧号浮标）：录制 = 选中的帧，冻结 = 抓取时刻，
	 * 实时 = 该源最新结果帧（GPU 结果晚两帧，不能用引擎当前帧号） */
	uint32_t RenderStatsPanel::CurrentDisplayFrameId(TimingSource source) const
	{
		if (m_RecordView)
			return ResolveCaptureFrameId();
		if (source == TimingSource::CPU)
			return m_CpuFrozen ? m_CpuFrozenFrameId : Profiler::Instance().GetFrameId();
		return m_GpuFrozen ? m_GpuFrozenFrameId : Profiler::Instance().GetGPUResultFrameId();
	}

	/* 图表弹窗：「在窗口中打开」弹出的独立顶层窗口（可移动 / 缩放 / 最大化，标题栏 X
	 * 可关）。跟着打开它的那个源走（标题 CPU / GPU Chart）；图表视图与卡片独立（两处切换条
	 * 各滑各的，下钻 / 折叠 / 缩放仍共享）。打开期间卡里的图表让位，收窗即恢复。 */
	void RenderStatsPanel::ShowTimingChartWindow()
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Editor);

		if (!m_ChartWindowOpen)
			return;

		ImGui::SetNextWindowPos(ImVec2(340.0f, 40.0f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(920.0f, 600.0f), ImGuiCond_FirstUseEver);
		if (m_ChartWindowMaximized)
		{
			/* 最大化：每帧强压到主视口工作区（跟随编辑器窗口尺寸变化；WorkPos 已扣掉
			 * 主菜单栏，不会盖住菜单）。被强制期间用户拖 / 拉都会在下帧弹回。 */
			const ImGuiViewport* viewport = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
			ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
		}
		else if (m_ChartWindowRestorePending)
		{
			/* 还原：把最大化前记下的浮动矩形写回（只此一帧，之后交回用户拖拽 / 缩放） */
			ImGui::SetNextWindowPos(ImVec2(m_ChartRestoreX, m_ChartRestoreY), ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(m_ChartRestoreW, m_ChartRestoreH), ImGuiCond_Always);
			m_ChartWindowRestorePending = false;
		}

		const TimingSource source = m_ChartWindowSource;
		TimingViewState& state = source == TimingSource::CPU ? m_CpuView : m_GpuView;
		const char* title = source == TimingSource::CPU ? "CPU Chart" : "GPU Chart";
		if (ImGui::Begin(title, &m_ChartWindowOpen))
		{
			if (source == TimingSource::CPU)
			{
				const CPUTimeScopeNode* display_root = ResolveCpuDisplayRoot();
				if (display_root == nullptr)
					ImGuiExt::DrawCommonTextUI("Timers", "No data");
				else
					ShowTimingAreaBody(TimingSource::CPU, *display_root, state, true);
			}
			else
			{
				const GPUTimeScopeNode* display_root = ResolveGpuDisplayRoot();
				if (display_root == nullptr)
					ImGuiExt::DrawCommonTextUI("Timers", "No data");
				else
					ShowTimingAreaBody(TimingSource::GPU, *display_root, state, true);
			}
		}
		ImGui::End();
	}

	/* 弹窗最大化 / 还原切换（行尾按钮触发）：最大化前先记下当前浮动矩形 —— 供还原写回
	 *（最大化期间窗口自身存的尺寸每帧被覆盖，不能指望 ImGui 记住浮动时的值）。 */
	void RenderStatsPanel::ToggleChartWindowMaximized()
	{
		if (!m_ChartWindowMaximized)
		{
			const ImVec2 pos = ImGui::GetWindowPos();
			const ImVec2 size = ImGui::GetWindowSize();
			m_ChartRestoreX = pos.x;
			m_ChartRestoreY = pos.y;
			m_ChartRestoreW = size.x;
			m_ChartRestoreH = size.y;
			m_ChartWindowMaximized = true;
		}
		else
		{
			m_ChartWindowMaximized = false;
			m_ChartWindowRestorePending = true;
		}
	}

	/* 抓取当前帧切换（卡 / 弹窗共用）：冻结拍下实时树快照 + 记下抓取时刻的帧号
	 *（弹窗的帧号浮标停在它上面，不再随实时跳动）；再点解冻回实时。 */
	void RenderStatsPanel::ToggleCaptureFrame(TimingSource source)
	{
		if (source == TimingSource::CPU)
		{
			m_CpuFrozen = !m_CpuFrozen;
			if (m_CpuFrozen)
			{
				const Profiler& profiler = Profiler::Instance();
				m_CpuFrozenRoot = profiler.GetCPURoot();
				m_CpuFrozenFrameId = profiler.GetFrameId();
			}
			return;
		}

		m_GpuFrozen = !m_GpuFrozen;
		if (m_GpuFrozen)
		{
			m_GpuFrozenRoot = Profiler::Instance().GetGPURoot();
			m_GpuFrozenFrameId = Profiler::Instance().GetGPUResultFrameId();
		}
	}

	/* 计时树搜索过滤（自检 / 外部驱动）：写输入缓冲并即时生效（空串 = 清空过滤） */
	void RenderStatsPanel::SetCPUTimingSearchFilter(const std::string& filter)
	{
		m_CpuView.Filter = ToLowercase(filter.c_str());
		std::snprintf(m_CpuView.Search, sizeof(m_CpuView.Search), "%s", filter.c_str());
	}

	void RenderStatsPanel::SetGPUTimingSearchFilter(const std::string& filter)
	{
		m_GpuView.Filter = ToLowercase(filter.c_str());
		std::snprintf(m_GpuView.Search, sizeof(m_GpuView.Search), "%s", filter.c_str());
	}

	/* 追踪文本会话：开关打开且采集未暂停时在写（状态驱动、每帧一次）—— 只收尾本面板开的
	 * 会话，不动外部手动开的。写盘是逐事件的重量级路径，所以不做常开，靠这个开关显式打开。 */
	void RenderStatsPanel::SyncCPUTraceTextSession()
	{
	#if HELIOS_PROFILE
		const bool capturing = Profiler::Instance().IsCPUEnabled() && Profiler::Instance().IsRecording();
		if (capturing && m_CpuTraceTextEnabled && !ProfilerTrace::IsSessionActive())
		{
			ProfilerTrace::BeginSession("Runtime", "PerfettoWriter-Runtime.json");
			m_CpuTraceTextSessionOwned = true;
		}
		else if ((!capturing || !m_CpuTraceTextEnabled) && m_CpuTraceTextSessionOwned)
		{
			ProfilerTrace::EndSession();
			m_CpuTraceTextSessionOwned = false;
		}
	#endif
	}


	/* 通道筛选下拉：按钮显示「开了几个 / 总共几个」，点开是复选。
	 * 关闭的通道在采集层就被跳过，不建节点 —— 树规模会跟着小。 */
	/* 通道表的位与显示名：下拉和自检共用同一份顺序 */
	constexpr ProfileChannel kChannelBits[] = {
		ProfileChannel::General, ProfileChannel::Render, ProfileChannel::Scene,
		ProfileChannel::UI, ProfileChannel::Assets, ProfileChannel::Jobs,
	};
	constexpr const char* kChannelNames[] = {
		"General", "Render", "Scene", "UI", "Assets", "Jobs",
	};
	constexpr uint32_t kChannelCount = static_cast<uint32_t>(std::size(kChannelBits));

	/* 开关一路通道：下拉点击与自检共用 */
	void RenderStatsPanel::ToggleChannel(uint32_t index)
	{
		if (index >= kChannelCount)
			return;

		const ProfileChannel bit = kChannelBits[index];
		m_ChannelFilter = HasChannel(m_ChannelFilter, bit)
			? (m_ChannelFilter & ~bit)
			: (m_ChannelFilter | bit);
	}

	void RenderStatsPanel::ShowChannelFilterPopup()
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Editor);
		
		const ProfileChannel filter = m_ChannelFilter;
		uint32_t enabled = 0;
		for (const ProfileChannel channel : kChannelBits)
		{
			if (HasChannel(filter, channel))
				++enabled;
		}

		if (ImGui::BeginPopup("##ChannelFilter"))
		{
			/* 行距贴紧：与工具栏的筛选菜单同一档 */
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 0.0f));

			if (PanelChrome::MenuItemSelectWithIcon(Icons::Id::None, "All", enabled == kChannelCount))
				m_ChannelFilter = ProfileChannel::All;
			if (PanelChrome::MenuItemSelectWithIcon(Icons::Id::None, "None", enabled == 0))
				m_ChannelFilter = ProfileChannel::None;
			ImGui::Separator();

			for (uint32_t index = 0; index < kChannelCount; ++index)
			{
				const bool on = HasChannel(filter, kChannelBits[index]);
				if (PanelChrome::MenuItemSelectWithIcon(Icons::Id::None, kChannelNames[index], on))
					ToggleChannel(index);
			}

			ImGui::PopStyleVar();
			ImGui::EndPopup();
		}
	}


	/* Treemap 下钻路径（标签序列，从显示根往下一层一个）：点击与自检共用 */
	void RenderStatsPanel::SetCpuTreemapZoom(const std::vector<std::string>& path)
	{
		m_CpuView.TreemapZoom = path;
	}

	void RenderStatsPanel::SetGpuTreemapZoom(const std::vector<std::string>& path)
	{
		m_GpuView.TreemapZoom = path;
	}

	/* 计时工具栏：追踪文本 + 暂停 / 继续、逐帧导航、峰值 / 谷值、清空、导出 —— 一行等大
	 * 图标按钮（宽度均分内容区）。暂停与清空对两条采集一起生效；逐帧与峰谷按当前看的树取数。 */
	void RenderStatsPanel::ShowTimingsToolbar()
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Editor);

		const Profiler& profiler = Profiler::Instance();
		const bool viewing_cpu = m_TimingSource == TimingSource::CPU;
		const uint32_t count = viewing_cpu ? profiler.GetCPUFrameCount() : profiler.GetGPUFrameCount();
		const int last_index = static_cast<int>(count) - 1;

		/* 暂停状态看 CPU 侧：两个录制一起停、一起开 */
		const bool paused = !profiler.IsRecording();

		const auto frame_id_at = [&](int index)
			{
				return viewing_cpu ? profiler.GetCPUFrameId(static_cast<uint32_t>(index))
					: profiler.GetGPUFrameId(static_cast<uint32_t>(index));
			};
		const auto frame_value_at = [&](int index)
			{
				return viewing_cpu ? profiler.GetCPUFrame(static_cast<uint32_t>(index)).TotalUs
					: profiler.GetGPUFrame(static_cast<uint32_t>(index)).TotalUs;
			};
		/* 选中帧在当前这棵树里的下标（找不到就落到最新一帧） */
		const auto selected_index = [&]()
			{
				const uint32_t selected = ResolveCaptureFrameId();
				const uint32_t hit = viewing_cpu ? profiler.FindCPUFrame(selected) : profiler.FindGPUFrame(selected);
				return hit == INVALID_SCOPE_INDEX ? last_index : static_cast<int>(hit);
			};

		const float button_gap = ImGui::GetStyle().ItemSpacing.x;
	#if HELIOS_PROFILE
		constexpr int kSlotCount = 8;
	#else
		constexpr int kSlotCount = 7;
	#endif
		const float button_width = (ImGui::GetContentRegionAvail().x - button_gap * (kSlotCount - 1))
			/ kSlotCount;
		const ImVec2 button_size(button_width, ImGui::GetFrameHeight() * 1.5f);

		/* 按钮按槽位排：追踪文本可能不编进来，用它统一处理首钮的 SameLine */
		bool first_slot = true;
		const auto next_slot = [&]()
			{
				if (!first_slot)
					ImGui::SameLine(0.0f, button_gap);
				first_slot = false;
			};

	#if HELIOS_PROFILE
		next_slot();
		if (Icons::IconButton(Icons::Id::TraceText, button_size, m_CpuTraceTextEnabled,
			m_CpuTraceTextEnabled
				? "Trace text: ON (writes PerfettoWriter-Runtime.json while capturing)"
				: "Trace text: OFF (also write per-function timings to a trace file)"))
			m_CpuTraceTextEnabled = !m_CpuTraceTextEnabled;
	#endif

		/* 暂停 / 继续：两条采集一起停，时间轴上留住的才是一段完整画面 */
		next_slot();
		if (paused)
		{
			if (Icons::IconButton(Icons::Id::Record, button_size, false, "Resume capture"))
			{
				Profiler::Instance().SetRecording(true);
			}
		}
		else if (Icons::IconButton(Icons::Id::Stop, button_size, false, "Pause capture (keep the buffered frames)"))
		{
			Profiler::Instance().SetRecording(false);
			/* 暂停后窗口不再滚动：钉住当前帧，便于逐帧细看 */
			m_ReplayFrameId = ResolveCaptureFrameId();
		}

		ImGui::BeginDisabled(count == 0);
		next_slot();
		if (Icons::IconButton(Icons::Id::PreviousFrame, button_size, false, "Previous frame"))
			m_ReplayFrameId = frame_id_at(ImMax(selected_index() - 1, 0));
		next_slot();
		if (Icons::IconButton(Icons::Id::NextFrame, button_size, false, "Next frame"))
			m_ReplayFrameId = frame_id_at(ImMin(selected_index() + 1, last_index));
		next_slot();
		if (Icons::IconButton(Icons::Id::Peak, button_size, false, "Peak frame"))
		{
			int best = 0;
			for (int index = 1; index <= last_index; ++index)
				if (frame_value_at(index) > frame_value_at(best))
					best = index;
			m_ReplayFrameId = frame_id_at(best);
		}
		next_slot();
		if (Icons::IconButton(Icons::Id::Valley, button_size, false, "Valley frame"))
		{
			int best = 0;
			for (int index = 1; index <= last_index; ++index)
				if (frame_value_at(index) < frame_value_at(best))
					best = index;
			m_ReplayFrameId = frame_id_at(best);
		}
		next_slot();
		if (Icons::IconButton(Icons::Id::Remove, button_size, false, "Clear the buffered frames"))
		{
			Profiler::Instance().ClearRecording();
			m_ReplayFrameId = 0;
		}
		ImGui::EndDisabled();

		/* 导出：GPU 录制写 CSV（宽表见 BuildRecordingCsv）；落盘路径交给系统保存对话框。
		 * 保存框不保证带后缀（macOS 面板不会自动加）—— 落盘前统一收尾补 ".csv"。 */
		ImGui::BeginDisabled(profiler.GetGPUFrameCount() == 0);
		next_slot();
		if (Icons::IconButton(Icons::Id::Export, button_size, false, "Export GPU recording (CSV)"))
		{
			const std::string path = ProfilerExport::EnsureRecordingExtension(FileDialog::SaveFile("csv(*.csv)\0*.csv\0"));
			const std::string csv = path.empty() ? std::string() : ProfilerExport::BuildRecordingCsv(profiler.GetGPURecorder());
			if (!csv.empty())
			{
				std::ofstream out(PathFromUtf8(path), std::ios::binary);
				out.write(csv.data(), static_cast<std::streamsize>(csv.size()));
				if (out.good())
					EDITOR_LOG_INFO("GPU recording exported: \"{}\" ({} frames).", path, profiler.GetGPUFrameCount());
				else
					EDITOR_LOG_ERROR("Failed to export GPU recording to \"{}\".", path);
			}
		}
		ImGui::EndDisabled();
	}

	/* 共用时间轴：CPU / GPU 两条曲线按引擎帧号对齐，一条滑条同时驱动两棵树。
	 * 域优先取 CPU 的帧号（每帧都有、连续），CPU 关着时退回 GPU。 */
	void RenderStatsPanel::ShowCaptureTimeline()
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Editor);

		const Profiler& profiler = Profiler::Instance();
		const bool gpu_enabled = profiler.IsGPUEnabled();
		const bool cpu_ready = profiler.IsCPUEnabled() && profiler.GetCPUFrameCount() > 0;
		const bool gpu_ready = gpu_enabled && profiler.GetGPUFrameCount() > 0;
		/* 帧号域跟着当前源走：GPU 结果晚两帧解析，用 CPU 的域选到最新一帧就永远没有数据 */
		const bool use_cpu = m_TimingSource == TimingSource::CPU ? cpu_ready : !gpu_ready;
		const uint32_t count = use_cpu ? profiler.GetCPUFrameCount() : (gpu_ready ? profiler.GetGPUFrameCount() : 0);
		if (count == 0)
			return;

		const auto frame_id_at = [&](uint32_t index)
			{ return use_cpu ? profiler.GetCPUFrameId(index) : profiler.GetGPUFrameId(index); };

		/* 建一次「域下标 → 各录制槽位」的映射：两边帧号都升序，双指针扫一遍就够。
		 * 之前是在画线的每个点里现查，600 帧的图上每帧几十万次线性比较，UI 直接顶到 5ms。 */
		const uint32_t cpu_count = profiler.GetCPUFrameCount();
		const uint32_t gpu_count = profiler.GetGPUFrameCount();
		std::vector<uint32_t> cpu_slot_at(count, INVALID_SCOPE_INDEX);
		std::vector<uint32_t> gpu_slot_at(count, INVALID_SCOPE_INDEX);
		{
			uint32_t cpu_cursor = 0;
			uint32_t gpu_cursor = 0;
			for (uint32_t index = 0; index < count; ++index)
			{
				const uint32_t frame_id = frame_id_at(index);
				while (cpu_cursor < cpu_count && profiler.GetCPUFrameId(cpu_cursor) < frame_id)
					++cpu_cursor;
				if (cpu_cursor < cpu_count && profiler.GetCPUFrameId(cpu_cursor) == frame_id)
					cpu_slot_at[index] = cpu_cursor;
				while (gpu_cursor < gpu_count && profiler.GetGPUFrameId(gpu_cursor) < frame_id)
					++gpu_cursor;
				if (gpu_cursor < gpu_count && profiler.GetGPUFrameId(gpu_cursor) == frame_id)
					gpu_slot_at[index] = gpu_cursor;
			}
		}

		const auto cpu_us_at = [&](uint32_t index, double& out_value)
			{
				const uint32_t hit = cpu_slot_at[index];
				if (hit == INVALID_SCOPE_INDEX)
					return false;
				out_value = profiler.GetCPUFrame(hit).TotalUs;
				return true;
			};
		const auto gpu_us_at = [&](uint32_t index, double& out_value)
			{
				if (!gpu_enabled)
					return false;
				const uint32_t hit = gpu_slot_at[index];
				if (hit == INVALID_SCOPE_INDEX)
					return false;
				/* 与 Frame 卡同口径：根节点就是该帧的通道区间并集（占用时长） */
				out_value = profiler.GetGPUFrame(hit).TotalUs;
				return true;
			};

		const int last_index = static_cast<int>(count) - 1;
		const ImGuiStyle& style = ImGui::GetStyle();

		/* ---- 两条曲线：尖峰分布 + CPU / GPU 谁在拖后腿；点击/拖动选帧 ---- */
		const float width = ImGui::GetContentRegionAvail().x;
		constexpr float kGraphHeight = 60.0f;
		const ImVec2 graph_min = ImGui::GetCursorScreenPos();
		const ImVec2 graph_max(graph_min.x + width, graph_min.y + kGraphHeight);

		ImGui::InvisibleButton("##CaptureTimeline", ImVec2(width, kGraphHeight));
		const bool graph_hovered = ImGui::IsItemHovered();

		ImDrawList* draw_list = ImGui::GetWindowDrawList();
		draw_list->AddRectFilled(graph_min, graph_max, ImGui::GetColorU32(EditorTheme::Token::Neutral1), 3.0f);
		draw_list->AddRect(graph_min, graph_max, ImGui::GetColorU32(EditorTheme::Token::Border), 3.0f);

		constexpr float kPadX = 4.0f;
		constexpr float kPadY = 3.0f;
		const ImVec2 plot_min(graph_min.x + kPadX, graph_min.y + kPadY);
		const ImVec2 plot_max(graph_max.x - kPadX, graph_max.y - kPadY);
		const float plot_width = ImMax(plot_max.x - plot_min.x, 1.0f);
		const float plot_height = ImMax(plot_max.y - plot_min.y, 1.0f);

		float value_max = 0.0f;
		for (uint32_t index = 0; index < count; ++index)
		{
			double value = 0.0;
			if (cpu_us_at(index, value))
				value_max = ImMax(value_max, static_cast<float>(value));
			if (gpu_us_at(index, value))
				value_max = ImMax(value_max, static_cast<float>(value));
		}
		value_max = ImMax(value_max * 1.15f, 1.0f);

		const auto column_x = [&](uint32_t index)
			{
				return count >= 2
					? plot_min.x + static_cast<float>(index) / static_cast<float>(count - 1) * plot_width
					: plot_min.x + plot_width * 0.5f;
			};
		const auto value_y = [&](double value)
			{ return plot_max.y - ImMin(static_cast<float>(value), value_max) / value_max * plot_height; };
		const auto index_at_mouse = [&]()
			{
				const float position = (ImGui::GetIO().MousePos.x - plot_min.x) / plot_width;
				return static_cast<int>(ImClamp(
					position * static_cast<float>(last_index), 0.0f, static_cast<float>(last_index)));
			};

		::Helios::Widgets::DrawBrokenSeries(draw_list, count,
			[&](uint32_t index) { double value = 0.0; return cpu_us_at(index, value); },
			[&](uint32_t index) { double value = 0.0; cpu_us_at(index, value); return ImVec2(column_x(index), value_y(value)); },
			ImGui::GetColorU32(::Helios::Widgets::kCpuLineColor), 1.2f);

		if (gpu_enabled)
		{
			::Helios::Widgets::DrawBrokenSeries(draw_list, count,
				[&](uint32_t index) { double value = 0.0; return gpu_us_at(index, value); },
				[&](uint32_t index) { double value = 0.0; gpu_us_at(index, value); return ImVec2(column_x(index), value_y(value)); },
				ImGui::GetColorU32(::Helios::Widgets::kGpuLineColor), 1.2f);
		}

		/* 上限刻度：左上角小字 */
		char scale_text[32];
		snprintf(scale_text, sizeof(scale_text), "%.0f us", value_max);
		draw_list->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.85f,
			ImVec2(plot_min.x + 3.0f, plot_min.y),
			ImGui::GetColorU32(EditorTheme::Token::TextDim), scale_text);

		/* 选中帧游标 */
		const uint32_t selected_id = ResolveCaptureFrameId();
		int selected_index = last_index;
		for (uint32_t index = 0; index < count; ++index)
		{
			if (frame_id_at(index) == selected_id)
			{
				selected_index = static_cast<int>(index);
				break;
			}
		}
		{
			const float cursor_x = column_x(static_cast<uint32_t>(selected_index));
			draw_list->AddLine(ImVec2(cursor_x, plot_min.y), ImVec2(cursor_x, plot_max.y),
				ImGui::GetColorU32(EditorTheme::Token::Accent), 1.0f);
		}

		/* 点击 / 拖动 = 选帧（按列就近取整） */
		if (ImGui::IsItemActive())
			m_ReplayFrameId = frame_id_at(static_cast<uint32_t>(index_at_mouse()));
		else if (graph_hovered)
		{
			const uint32_t index = static_cast<uint32_t>(index_at_mouse());
			double cpu_value = 0.0;
			double gpu_value = 0.0;
			ImGui::BeginTooltip();
			ImGui::Text("Frame #%u", frame_id_at(index));
			if (cpu_us_at(index, cpu_value))
				ImGui::Text("CPU  %.0f us", cpu_value);
			if (gpu_us_at(index, gpu_value))
				ImGui::Text("GPU  %.0f us", gpu_value);
			ImGui::EndTooltip();
		}

		/* ---- 选帧滑条：图表下方 ---- */
		if (last_index > 0)
		{
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
				ImVec2(style.ItemSpacing.x, style.ItemSpacing.y * 0.5f));
			int frame_value = selected_index;
			if (::Helios::Widgets::FrameSlider("##CaptureFrame", frame_value, 0, last_index))
				m_ReplayFrameId = frame_id_at(static_cast<uint32_t>(frame_value));
			ImGui::PopStyleVar();
		}

		/* ---- 概要：帧数 + 峰值帧 ---- */
		int peak_index = 0;
		double peak_value = 0.0;
		for (uint32_t index = 0; index < count; ++index)
		{
			double value = 0.0;
			if (!cpu_us_at(index, value) && !gpu_us_at(index, value))
				continue;
			if (value > peak_value)
			{
				peak_value = value;
				peak_index = static_cast<int>(index);
			}
		}
		ImGui::TextDisabled("%u frames   peak %.0f us @ #%u", count, peak_value,
			frame_id_at(static_cast<uint32_t>(peak_index)));
	}

	/* GPU 树的树表视图：通道 / 作用域逐层耗时（无数据节点以占位显示） */
	void RenderStatsPanel::ShowTimingTree(const GPUTimeScopeNode& root)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Editor);

		/* 命中计数每帧随树结算（搜索框在工具条、悬浮提示用；自检可读） */
		m_GpuView.MatchCount = TimingMatchCount(root, m_GpuView.Filter);
		const bool filtering = !m_GpuView.Filter.empty();

		std::function<void(const GPUTimeScopeNode&)> show_node_recursively;
		show_node_recursively = [&show_node_recursively, filtering, this](
			const GPUTimeScopeNode& timer_node)
			{
				if (timer_node.Label.empty())
					return;

				/* 搜索过滤：自身与后代都无命中的整支不画（过滤清空时恒可见） */
				if (filtering && !TimingSubtreeMatches(timer_node, m_GpuView.Filter))
					return;

				char label[64] = {};
				snprintf(label, sizeof(label), "(%d) %s",
					static_cast<int>(timer_node.QueryIndex), timer_node.Label.c_str());
				label[sizeof(label) - 1] = 0;

				const bool has_children = !timer_node.Children.empty();

				/* 浅层默认展开；点行任意一处（含箭头那一下）都选中这行
				 *（无效 QueryIndex 转 int 恰是 -1 = 未选中哨兵，天然对得上）；
				 * 过滤中：命中路径上的祖父行强制展开 */
				const TimingRowState row = TimingTreeRow(
					reinterpret_cast<void*>(static_cast<intptr_t>(timer_node.QueryIndex)),
					label, has_children, true,
					m_GpuView.SelectedNode == static_cast<int>(timer_node.QueryIndex),
					filtering && !TimingNodeMatches(timer_node, m_GpuView.Filter));
				if (row.Clicked)
					m_GpuView.SelectedNode = static_cast<int>(timer_node.QueryIndex);

				ImGui::TableNextColumn();
				if (timer_node.HasData)
					ImGui::Text("%.2f", timer_node.TotalUs);
				else
					ImGui::TextDisabled("%s", kNoDataText);

				/* 悬停显示阶段占用（看清是顶点还是片元把 GPU 占住了）。三段各取并集，
				 * 之间仍会交叠，加起来可能大于本行时长。只列有值的阶段 —— 计算派发
				 * 只有 compute，渲染通道没有 compute。 */
				if (timer_node.HasData && timer_node.HasStageSplit && ImGui::IsItemHovered())
				{
					ImGui::BeginTooltip();
					if (timer_node.StageVertexUs > 0.0)
						ImGui::Text("vertex   %.2f us", timer_node.StageVertexUs);
					if (timer_node.StageFragmentUs > 0.0)
						ImGui::Text("fragment %.2f us", timer_node.StageFragmentUs);
					if (timer_node.StageComputeUs > 0.0)
						ImGui::Text("compute  %.2f us", timer_node.StageComputeUs);
					ImGui::EndTooltip();
				}

				if (row.Open && has_children)
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
			show_node_recursively(root);
			ImGui::EndTable();
		}
	}

	/* GPU 录制导出（CSV 宽表：一行一帧）：帧号 · 根总耗时（v / f 细分）· 每个作用域节点两列
	 * （总耗时 + 顶点细分，片元 = 总 − 顶点）。列名 = 标签路径 + ".total_us" / ".vertex_us"；
	 * 节点集合取全部录制帧的并集（缺失留空）、列序 = 首次出现的先序。纯函数：headless 能直接断言。 */
	
}
