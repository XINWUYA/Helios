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

		/* 滑动页签：两个等宽页签共用一条底槽，选中块在两者之间平滑滑动（指数趋近）。
		 * slide 由调用方持久化（0 = 左页签、1 = 右页签），切换时产生滑动动画；
		 * 返回本帧被点击的页签下标（-1 = 未点击）。 */
		int SlidingTabPair(const char* id, const char* label_left, Icons::Id icon_left,
			const char* label_right, Icons::Id icon_right, bool right_selected, float& slide)
		{
			const ImGuiStyle& style = ImGui::GetStyle();
			const float height = ImGui::GetFrameHeight();
			const float width = ImGui::GetContentRegionAvail().x;
			const float seg_width = width * 0.5f;
			const ImVec2 pos = ImGui::GetCursorScreenPos();

			ImGui::PushID(id);
			ImGui::InvisibleButton("##left", ImVec2(seg_width, height));
			const bool click_left = ImGui::IsItemClicked();
			const bool hover_left = ImGui::IsItemHovered();
			ImGui::SameLine(0.0f, 0.0f);
			ImGui::InvisibleButton("##right", ImVec2(width - seg_width, height));
			const bool click_right = ImGui::IsItemClicked();
			const bool hover_right = ImGui::IsItemHovered();
			ImGui::PopID();

			/* 选中块的滑动：指数趋近（约 0.1s 滑到位），到目标即停避免长期微动 */
			const float target = right_selected ? 1.0f : 0.0f;
			const float dt = ImMin(ImGui::GetIO().DeltaTime, 0.1f);
			slide += (target - slide) * ImMin(1.0f, dt * 18.0f);
			if (ImFabs(slide - target) < 0.002f)
				slide = target;

			ImDrawList* draw_list = ImGui::GetWindowDrawList();
			draw_list->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height),
				ImGui::GetColorU32(EditorTheme::Token::Neutral4), style.FrameRounding);
			draw_list->AddRect(pos, ImVec2(pos.x + width, pos.y + height),
				ImGui::GetColorU32(EditorTheme::Token::Border), style.FrameRounding, 0, 1.0f);

			/* 未选中格悬停时的提亮底：画在滑动块之下 */
			if (hover_left && right_selected)
				draw_list->AddRectFilled(pos, ImVec2(pos.x + seg_width, pos.y + height),
					ImGui::GetColorU32(EditorTheme::Token::Neutral5), style.FrameRounding);
			if (hover_right && !right_selected)
				draw_list->AddRectFilled(ImVec2(pos.x + seg_width, pos.y), ImVec2(pos.x + width, pos.y + height),
					ImGui::GetColorU32(EditorTheme::Token::Neutral5), style.FrameRounding);

			/* 选中块（滑动中：位置在两格之间插值） */
			constexpr float kBlockInset = 2.0f;
			const float block_width = seg_width - kBlockInset * 2.0f;
			const float block_x = pos.x + kBlockInset + seg_width * slide;
			const ImVec2 block_min(block_x, pos.y + kBlockInset);
			const ImVec2 block_max(block_x + block_width, pos.y + height - kBlockInset);
			const float block_rounding = ImMax(1.0f, style.FrameRounding - 1.0f);
			draw_list->AddRectFilled(block_min, block_max,
				ImGui::GetColorU32(EditorTheme::WithAlpha(EditorTheme::Token::Accent, 0.17f)), block_rounding);
			draw_list->AddRect(block_min, block_max,
				ImGui::GetColorU32(EditorTheme::WithAlpha(EditorTheme::Token::Accent, 0.65f)),
				block_rounding, 0, 1.0f);

			/* 页签内容（图标 + 文字，格内居中）：激活侧高亮 */
			const auto draw_tab = [&](float seg_x, const char* label, Icons::Id icon, bool active, bool hovered)
				{
					const float icon_size = height * 0.62f;
					constexpr float kIconGap = 6.0f;
					const ImVec2 label_size = ImGui::CalcTextSize(label);
					const float content_width = icon_size + kIconGap + label_size.x;
					const float content_x = seg_x + (seg_width - content_width) * 0.5f;
					const ImU32 content_color = ImGui::GetColorU32(
						active || hovered ? EditorTheme::Token::Text : EditorTheme::Token::TextDim);
					Icons::Draw(draw_list, icon,
						ImVec2(content_x + icon_size * 0.5f, pos.y + height * 0.5f), icon_size, content_color);
					draw_list->AddText(ImVec2(content_x + icon_size + kIconGap,
						pos.y + (height - label_size.y) * 0.5f), content_color, label);
				};
			draw_tab(pos.x, label_left, icon_left, !right_selected, hover_left);
			draw_tab(pos.x + seg_width, label_right, icon_right, right_selected, hover_right);

			if (click_left)
				return 0;
			if (click_right)
				return 1;
			return -1;
		}

		/* 帧号滑条（录制回放的选帧）：外观同标准滑条 —— 圆角轨道 + 滑块，颜色取
		 * 主题的 FrameBg / SliderGrab 档；帧号文本画在滑动块中（块宽按最大帧号的
		 * 文本宽预留，读数始终完整）。交互：点击跳转 / 拖动选帧。返回是否被改动。 */
		bool FrameSlider(const char* id, int& value, int v_min, int v_max)
		{
			const ImGuiStyle& style = ImGui::GetStyle();
			const float width = ImGui::GetContentRegionAvail().x;
			const float height = ImGui::GetFrameHeight();
			const ImVec2 pos = ImGui::GetCursorScreenPos();

			ImGui::PushID(id);
			ImGui::InvisibleButton("##slider", ImVec2(width, height));
			const bool active = ImGui::IsItemActive();
			const bool hovered = ImGui::IsItemHovered();
			ImGui::PopID();

			/* 块宽按"最大帧号"的文本预留：任意帧的读数都能完整显示在块内 */
			char widest_label[32];
			snprintf(widest_label, sizeof(widest_label), "Frame %d", v_max);

			constexpr float kGrabPadding = 2.0f; /* 与 ImGui 标准滑条的块内边距一致 */
			const float grab_width = ImGui::CalcTextSize(widest_label).x + style.FramePadding.x * 2.0f;
			const float usable = ImMax(width - grab_width - kGrabPadding * 2.0f, 1.0f);

			/* 交互：点击跳转 / 拖动选帧（按住不放连续更新） */
			bool value_changed = false;
			if (active)
			{
				const float ratio = ImClamp((ImGui::GetIO().MousePos.x - pos.x
					- kGrabPadding - grab_width * 0.5f) / usable, 0.0f, 1.0f);
				const int new_value = v_min + static_cast<int>(ratio * (v_max - v_min) + 0.5f);
				if (new_value != value)
				{
					value = new_value;
					value_changed = true;
				}
			}

			/* 绘制：轨道（标准滑条的框色档 + 边框）+ 滑动块 + 块内帧号 */
			const float ratio = v_max > v_min
				? static_cast<float>(value - v_min) / static_cast<float>(v_max - v_min) : 0.0f;
			const ImVec2 grab_min(pos.x + kGrabPadding + ratio * usable, pos.y + kGrabPadding);
			const ImVec2 grab_max(grab_min.x + grab_width, pos.y + height - kGrabPadding);

			ImDrawList* draw_list = ImGui::GetWindowDrawList();
			draw_list->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height),
				ImGui::GetColorU32(active ? ImGuiCol_FrameBgActive
					: hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg),
				style.FrameRounding);
			draw_list->AddRect(pos, ImVec2(pos.x + width, pos.y + height),
				ImGui::GetColorU32(ImGuiCol_Border), style.FrameRounding, 0, style.FrameBorderSize);
			draw_list->AddRectFilled(grab_min, grab_max,
				ImGui::GetColorU32(active ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab),
				style.GrabRounding);

			char label[32];
			snprintf(label, sizeof(label), "Frame %d", value);
			const ImVec2 label_size = ImGui::CalcTextSize(label);
			draw_list->AddText(
				ImVec2(grab_min.x + (grab_width - label_size.x) * 0.5f,
					pos.y + (height - label_size.y) * 0.5f),
				ImGui::GetColorU32(ImGuiCol_Text), label);

			return value_changed;
		}
	}

	void RenderStatsPanel::OnImGuiRender()
	{
		PROFILE_FUNCTION();

		ImGui::Begin(Panel::kStatInfo);
		{
			/* 面板标题由页签承担（图标 + "Stat Info"），面板内不再重复一行 */

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
			{
				RenderQueryProfiler::Instance().SetEnabled(timer_enabled);
				/* 计时关闭：收回录制 —— 观测方式与录制控件一并隐藏，不让录制在
				 * 看不见的地方继续（已录内容保留，重开后可继续回放） */
				if (!timer_enabled)
					RenderQueryProfiler::Instance().GetRecorder().Stop();
			}

			GPUTimerRecorder& recorder = RenderQueryProfiler::Instance().GetRecorder();

			/* 观测方式：两个滑动页签（实时 / 录制）——选中块在两页签间平滑滑动；
			 * 计时关闭时整组隐藏（只剩开关与占位） */
			if (timer_enabled)
			{
				const int switched = SlidingTabPair("##ViewTabs", "Live", Icons::Id::Live,
					"Record", Icons::Id::Record, m_RecordView, m_TabSlide);
				if (switched == 0)
					m_RecordView = false;
				else if (switched == 1 && !m_RecordView)
				{
					m_RecordView = true;
					/* 切入时若已有录制内容：停在最后一帧 */
					if (!recorder.IsRecording() && recorder.GetFrameCount() > 0)
						m_ReplayFrame = static_cast<int>(recorder.GetFrameCount()) - 1;
				}
			}

			/* 录制工具栏：录制 / 停止、逐帧导航、峰值 / 谷值、清空 —— 一行等大图标按钮。
			 * 录制视图常显；实时视图只在录制进行中显示（不让录制状态失联）。 */
			if (timer_enabled && (m_RecordView || recorder.IsRecording()))
				ShowGPUTimerToolbar();

			/* 录制进度：录制中显示帧数 + 细进度条（红 = 录制中） */
			if (timer_enabled && recorder.IsRecording())
			{
				const uint32_t count_now = recorder.GetFrameCount();
				ImGui::TextDisabled("Recording %u / %u", count_now, GPUTimerRecorder::MAX_FRAMES);

				const float progress = static_cast<float>(count_now)
					/ static_cast<float>(GPUTimerRecorder::MAX_FRAMES);
				const ImVec2 bar_min = ImGui::GetCursorScreenPos();
				const float bar_width = ImGui::GetContentRegionAvail().x;
				constexpr float kBarHeight = 3.0f;
				ImDrawList* draw_list = ImGui::GetWindowDrawList();
				draw_list->AddRectFilled(bar_min, ImVec2(bar_min.x + bar_width, bar_min.y + kBarHeight),
					ImGui::GetColorU32(EditorTheme::Token::Neutral4), kBarHeight * 0.5f);
				draw_list->AddRectFilled(bar_min, ImVec2(bar_min.x + bar_width * progress, bar_min.y + kBarHeight),
					ImGui::GetColorU32(EditorTheme::Token::Danger), kBarHeight * 0.5f);
				ImGui::Dummy(ImVec2(bar_width, kBarHeight));
			}

			/* 工具栏可能清了内容 / 开了新录制：取最新帧数再归位选中帧 */
			const uint32_t recorded_count = recorder.GetFrameCount();
			if (m_ReplayFrame >= static_cast<int>(recorded_count))
				m_ReplayFrame = -1;

			/* 录制刚结束（手动 / 录满 / 计时关闭）且在看录制视图：停在最后一帧 */
			if (m_RecordView && m_WasRecording && !recorder.IsRecording() && recorded_count > 0)
				m_ReplayFrame = static_cast<int>(recorded_count) - 1;
			m_WasRecording = recorder.IsRecording();

			/* 录制视图 + 有内容 + 不在录制中：未选中时落到最后一帧 */
			if (m_RecordView && !recorder.IsRecording() && recorded_count > 0 && m_ReplayFrame < 0)
				m_ReplayFrame = static_cast<int>(recorded_count) - 1;

			/* 逐帧回放控件：录制视图 + 有录制内容 + 不在录制中 */
			const bool replaying = timer_enabled && m_RecordView && !recorder.IsRecording()
				&& recorded_count > 0 && m_ReplayFrame >= 0;
			if (replaying)
				ShowGPUTimerRecordingUI();

			/* 显示根：录制视图 = 选中录制帧（录制中 / 无内容不显示树）；其余 = 实时树 */
			const ResultGPUTimerNode* display_root = nullptr;
			if (replaying)
				display_root = &recorder.GetFrame(static_cast<uint32_t>(m_ReplayFrame));
			else if (!m_RecordView)
				display_root = &Renderer::GetGPUTimerRoot();

			if (!timer_enabled)
			{
				/* 计时关闭：观测方式与录制控件已隐藏，只剩开关与占位 */
				ImGuiExt::DrawCommonTextUI("Timers", "Off");
			}
			else if (m_RecordView && !replaying)
			{
				/* 录制视图：录制中 / 无内容 —— 不显示实时统计，给状态与用法提示 */
				ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
				if (recorder.IsRecording())
					ImGui::TextWrapped("Frame timings will appear here when recording finishes.");
				else
					ImGui::TextWrapped("Press Record to capture up to %u frames of per-pass GPU timings. "
						"Recording stops when full, or press Stop at any time. When finished, browse "
						"frames above and inspect each frame's timings here.", GPUTimerRecorder::MAX_FRAMES);
				ImGui::PopStyleColor();
			}
			else if (display_root == nullptr || display_root->Label.empty())
			{
				ImGuiExt::DrawCommonTextUI("Timers", "No data");
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
						 * 分清"顶点瓶颈"还是"片元 / 带宽开销"）——仅通道采样后端
						 * （Metal）提供细分；桌面 GL 无分阶段采样点，不显示此提示 */
						if (timer_node.HasData && timer_node.HasStageSplit && ImGui::IsItemHovered())
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
					show_node_recursively(*display_root);
					ImGui::EndTable();
				}
			}
		}

		PanelChrome::EndCard(card);
	}

	/* 录制工具栏：录制 / 停止、上一帧 / 下一帧、峰值 / 谷值、清空 —— 一行等大图标按钮
	 * （宽度均分内容区；无内容 / 录制中时回放按钮置灰）。 */
	void RenderStatsPanel::ShowGPUTimerToolbar()
	{
		PROFILE_FUNCTION();

		GPUTimerRecorder& recorder = RenderQueryProfiler::Instance().GetRecorder();
		const uint32_t count = recorder.GetFrameCount();
		const bool recording = recorder.IsRecording();
		const int last_index = static_cast<int>(count) - 1;

		const float button_gap = ImGui::GetStyle().ItemSpacing.x;
		const float button_width = (ImGui::GetContentRegionAvail().x - button_gap * 5.0f) / 6.0f;
		const ImVec2 button_size(button_width, ImGui::GetFrameHeight() * 1.5f);

		/* 录制 / 停止（同一按钮位，按状态换图标与提示） */
		if (recording)
		{
			if (Icons::IconButton(Icons::Id::Stop, button_size, false, "Stop recording (keep captured frames)"))
				recorder.Stop();
		}
		else if (Icons::IconButton(Icons::Id::Record, button_size, false, "Start recording (per-frame raw values)"))
		{
			recorder.Start();
			m_ReplayFrame = -1; /* 新录制从空内容开始 */
		}

		/* 回放按钮：无内容 / 录制中不可用 */
		ImGui::BeginDisabled(recording || count == 0);
		ImGui::SameLine(0.0f, button_gap);
		if (Icons::IconButton(Icons::Id::PreviousFrame, button_size, false, "Previous frame"))
			m_ReplayFrame = m_ReplayFrame >= 0 ? ImMax(m_ReplayFrame - 1, 0) : last_index;
		ImGui::SameLine(0.0f, button_gap);
		if (Icons::IconButton(Icons::Id::NextFrame, button_size, false, "Next frame"))
			m_ReplayFrame = m_ReplayFrame >= 0 ? ImMin(m_ReplayFrame + 1, last_index) : last_index;
		ImGui::SameLine(0.0f, button_gap);
		if (Icons::IconButton(Icons::Id::Peak, button_size, false, "Peak frame"))
		{
			int best = 0;
			for (int index = 1; index <= last_index; ++index)
				if (recorder.GetFrame(static_cast<uint32_t>(index)).GPUTime
					> recorder.GetFrame(static_cast<uint32_t>(best)).GPUTime)
					best = index;
			m_ReplayFrame = best;
		}
		ImGui::SameLine(0.0f, button_gap);
		if (Icons::IconButton(Icons::Id::Valley, button_size, false, "Valley frame"))
		{
			int best = 0;
			for (int index = 1; index <= last_index; ++index)
				if (recorder.GetFrame(static_cast<uint32_t>(index)).GPUTime
					< recorder.GetFrame(static_cast<uint32_t>(best)).GPUTime)
					best = index;
			m_ReplayFrame = best;
		}
		ImGui::SameLine(0.0f, button_gap);
		if (Icons::IconButton(Icons::Id::Remove, button_size, false, "Clear recording"))
		{
			recorder.Clear();
			m_ReplayFrame = -1;
		}
		ImGui::EndDisabled();
	}

	/* GPU 录制的逐帧回放：帧间总耗时曲线（点击/拖动选帧 + 上限刻度）→ 选帧滑条
	 * （图表下方；外观同标准滑条，帧号显示在滑动块中）→ 概要行（帧数 / 峰值帧）。
	 * 录制树里是逐帧原始值（无平滑）—— 逐帧分析看的就是每一帧的真值。 */
	void RenderStatsPanel::ShowGPUTimerRecordingUI()
	{
		PROFILE_FUNCTION();

		const GPUTimerRecorder& recorder = RenderQueryProfiler::Instance().GetRecorder();
		const uint32_t count = recorder.GetFrameCount();
		if (count == 0)
			return; /* 防御：调用方已按"有内容"判断，这里兜住帧内状态变化 */
		const int last_index = static_cast<int>(count) - 1;
		const ImGuiStyle& style = ImGui::GetStyle();

		/* ---- 帧间总耗时曲线：一眼看出尖峰分布；点击/拖动选帧 ---- */
		const float width = ImGui::GetContentRegionAvail().x;
		constexpr float kGraphHeight = 60.0f;
		const ImVec2 graph_min = ImGui::GetCursorScreenPos();
		const ImVec2 graph_max(graph_min.x + width, graph_min.y + kGraphHeight);

		ImGui::InvisibleButton("##GPURecordingGraph", ImVec2(width, kGraphHeight));
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
			value_max = ImMax(value_max, static_cast<float>(recorder.GetFrame(index).GPUTime));
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

		DrawBrokenSeries(draw_list, count,
			[](uint32_t) { return true; },
			[&](uint32_t index) { return ImVec2(column_x(index), value_y(recorder.GetFrame(index).GPUTime)); },
			ImGui::GetColorU32(kGpuLineColor), 1.2f);

		/* 上限刻度：左上角小字（与 Frame 卡曲线同一笔账） */
		char scale_text[32];
		snprintf(scale_text, sizeof(scale_text), "%.0f us", value_max);
		draw_list->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.85f,
			ImVec2(plot_min.x + 3.0f, plot_min.y),
			ImGui::GetColorU32(EditorTheme::Token::TextDim), scale_text);

		/* 选中帧游标 */
		if (m_ReplayFrame >= 0)
		{
			const float cursor_x = column_x(static_cast<uint32_t>(m_ReplayFrame));
			draw_list->AddLine(ImVec2(cursor_x, plot_min.y), ImVec2(cursor_x, plot_max.y),
				ImGui::GetColorU32(EditorTheme::Token::Accent), 1.0f);
		}

		/* 点击 / 拖动 = 选帧（按列就近取整） */
		if (ImGui::IsItemActive())
			m_ReplayFrame = index_at_mouse();
		else if (graph_hovered)
		{
			const int index = index_at_mouse();
			ImGui::BeginTooltip();
			ImGui::Text("Frame #%u", recorder.GetFrameId(static_cast<uint32_t>(index)));
			ImGui::Text("%.0f us", recorder.GetFrame(static_cast<uint32_t>(index)).GPUTime);
			ImGui::EndTooltip();
		}

		/* ---- 选帧滑条：图表下方（外观同标准滑条；帧号显示在滑动块中） ---- */
		if (last_index > 0)
		{
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
				ImVec2(style.ItemSpacing.x, style.ItemSpacing.y * 0.5f));
			int frame_value = m_ReplayFrame >= 0 ? m_ReplayFrame : last_index;
			if (FrameSlider("##GPURecordingFrame", frame_value, 0, last_index))
				m_ReplayFrame = frame_value;
			ImGui::PopStyleVar();
		}

		/* ---- 概要：帧数 + 峰值帧（最慢在哪一眼可见） ---- */
		int peak_index = 0;
		for (uint32_t index = 1; index < count; ++index)
			if (recorder.GetFrame(index).GPUTime
				> recorder.GetFrame(static_cast<uint32_t>(peak_index)).GPUTime)
				peak_index = static_cast<int>(index);
		ImGui::TextDisabled("%u frames   peak %.0f us @ #%u", count,
			recorder.GetFrame(static_cast<uint32_t>(peak_index)).GPUTime,
			recorder.GetFrameId(static_cast<uint32_t>(peak_index)));
	}
}
