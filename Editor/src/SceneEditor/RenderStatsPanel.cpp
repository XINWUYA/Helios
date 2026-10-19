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
#include "EditorIcons.h"
#include "PanelChrome.h"
#include "PanelRegistry.h"
#include "Helios/Application/FileDialog.h"
#include "Helios/Common/PathUtils.h"
#include "Helios/Core/CPUTimerProfiler.h"
#include "Helios/Core/FrameTimeProfiler.h"
#include "Helios/Core/Logger.h"
#include "Helios/ImGui/EditorTheme.h"
#include "EditorCommon.h"
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
				ImGui::GetColorU32(EditorTheme::Token::Border), style.FrameRounding, 0, Scaled(1.0f));

			/* 未选中格悬停时的提亮底：画在滑动块之下 */
			if (hover_left && right_selected)
				draw_list->AddRectFilled(pos, ImVec2(pos.x + seg_width, pos.y + height),
					ImGui::GetColorU32(EditorTheme::Token::Neutral5), style.FrameRounding);
			if (hover_right && !right_selected)
				draw_list->AddRectFilled(ImVec2(pos.x + seg_width, pos.y), ImVec2(pos.x + width, pos.y + height),
					ImGui::GetColorU32(EditorTheme::Token::Neutral5), style.FrameRounding);

			/* 选中块（滑动中：位置在两格之间插值） */
			const float kBlockInset = Scaled(2.0f);
			const float block_width = seg_width - kBlockInset * 2.0f;
			const float block_x = pos.x + kBlockInset + seg_width * slide;
			const ImVec2 block_min(block_x, pos.y + kBlockInset);
			const ImVec2 block_max(block_x + block_width, pos.y + height - kBlockInset);
			const float block_rounding = ImMax(Scaled(1.0f), style.FrameRounding - Scaled(1.0f));
			draw_list->AddRectFilled(block_min, block_max,
				ImGui::GetColorU32(EditorTheme::WithAlpha(EditorTheme::Token::Accent, 0.17f)), block_rounding);
			draw_list->AddRect(block_min, block_max,
				ImGui::GetColorU32(EditorTheme::WithAlpha(EditorTheme::Token::Accent, 0.65f)),
				block_rounding, 0, Scaled(1.0f));

			/* 页签内容（图标 + 文字，格内居中）：激活侧高亮 */
			const auto draw_tab = [&](float seg_x, const char* label, Icons::Id icon, bool active, bool hovered)
				{
					const float icon_size = height * 0.62f;
					const float kIconGap = Scaled(6.0f);
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

			const float kGrabPadding = Scaled(2.0f); /* 与 ImGui 标准滑条的块内边距一致 */
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

			/* 行底：选中 = 强调色 40%；悬停 = 白 12%；选中被悬停仍保住紫底（RowHoverSelected，
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

		/* 「计时树」搜索行（CPU / GPU 卡共用）：放大镜输入框（输入即更新过滤词，空 = 不过滤）
		 * + 行尾命中计数（只在过滤中显示，"0 命中"也有反馈）。filter 写回后由调用方重画。 */
		void TimingSearchRow(const char* id, char* buffer, size_t buffer_size,
			std::string& filter, uint32_t match_count, float trailing_width = 0.0f)
		{
			char count_text[32] = {};
			float count_width = 0.0f;
			if (!filter.empty())
			{
				std::snprintf(count_text, sizeof(count_text), "%u matched", match_count);
				count_width = ImGui::CalcTextSize(count_text).x + ImGui::GetStyle().ItemSpacing.x;
			}

			Icons::BeginSearchInput();
			ImGui::SetNextItemWidth(-1.0f - count_width - trailing_width);
			if (ImGui::InputTextWithHint(id, "Search scopes...", buffer, buffer_size))
				filter = ToLowercase(buffer);
			Icons::EndSearchInput();

			if (!filter.empty())
			{
				ImGui::SameLine();
				ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
				ImGui::TextUnformatted(count_text);
				ImGui::PopStyleColor();
			}
		}

		/* 同行内绝对定位到「行首起 offset 处」：SameLine 与 SetCursorPosX 的基准都是窗口外缘
		 *（imgui.cpp：Pos.x − Scroll.x + x，不含缩进），直接拿来整行会偏一个 WindowPadding。
		 * 这里补上 Indent + ColumnsOffset，与普通行首同基准。 */
		void SameLineAt(float offset_from_line_start)
		{
			ImGui::SameLine(0.0f, 0.0f);
			const ImGuiWindow* window = ImGui::GetCurrentWindow();
			ImGui::SetCursorPosX(window->DC.Indent.x + window->DC.ColumnsOffset.x + offset_from_line_start);
		}

		/* 视图切换器（三格）的紧凑宽度：三段各 = 行高 + 8（行太窄时让位 —— 见卡 / 窗行内
		 * 的 ImMin，搜索至少留 70） */
		float TimingSwitcherWidth()
		{
			return (ImGui::GetFrameHeight() + 8.0f) * 3.0f;
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

		/* 「悬浮读数」的位置行：插桩点"文件名:行号"（__FILE__ 是编译时路径，取基名；
		 * 无位置信息的节点 —— 合成帧根 / 直接构造的 —— 不画）。 */
		void TimingLocationText(const ResultCPUTimerNode& node)
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

		/* ---- CPU 图表（Treemap / 火焰图）公共件 ---- */

		/* 图表条目的耗时文本（火焰图条目标签用）：≥1000us 转 ms（单行要短） */
		void FormatBlockTime(double us, char* out, size_t out_size)
		{
			if (us >= 1000.0)
				std::snprintf(out, out_size, "%.2f ms", us / 1000.0);
			else
				std::snprintf(out, out_size, "%.1f us", us);
		}

		/* 热力强度（0..1）：按 log10(µs) 分档 —— 跨帧稳定、跨函数可比（深色主题越热越亮） */
		float HeatMixForUs(double total_us)
		{
			const double clamped = total_us < 1.0 ? 1.0 : total_us;
			return static_cast<float>(ImClamp((std::log10(clamped) - 2.0) / 2.5, 0.0, 1.0));
		}

		ImU32 HeatFillForUs(double total_us)
		{
			return ImGui::GetColorU32(ImLerp(EditorTheme::Token::CardBg, EditorTheme::Token::Accent,
				HeatMixForUs(total_us)));
		}

		/* 热力色块上的文字：亮底（后半段）用深字、暗底用正文色 */
		ImU32 HeatTextForUs(double total_us)
		{
			return ImGui::GetColorU32(HeatMixForUs(total_us) >= 0.55f
				? EditorTheme::Token::Neutral0 : EditorTheme::Token::Text);
		}

		/* squarified treemap：权重降序的项铺进 [x,y,w,h]、块尽量方正（贪心成行）。
		 * 输出与输入同序的矩形；权重 ≤ 0.5 的项不参与（保持零尺寸）。 */
		void SquarifyInto(const std::vector<float>& weights, float x, float y, float width, float height,
			std::vector<ImVec2>& out_pos, std::vector<ImVec2>& out_size)
		{
			const size_t count = weights.size();
			out_pos.assign(count, ImVec2(0.0f, 0.0f));
			out_size.assign(count, ImVec2(0.0f, 0.0f));
			if (count == 0 || width <= 0.0f || height <= 0.0f)
				return;

			float total = 0.0f;
			for (const float weight : weights)
				total += weight > 0.5f ? weight : 0.0f;
			if (total <= 0.0f)
				return;

			std::vector<size_t> order(count);
			for (size_t index = 0; index < count; ++index)
				order[index] = index;
			std::stable_sort(order.begin(), order.end(),
				[&weights](size_t left, size_t right) { return weights[left] > weights[right]; });

			/* 试排 [start..end] 成一行的最差长宽比（squarified 贪心判据）：宽矩形下
			 * 行为左侧竖条、矮矩形下行为顶部横条 */
			const auto worst_aspect = [&weights, &order](size_t start, size_t end, float row_sum,
				float rest, float rw, float rh)
				{
					const float row_area = row_sum / rest * rw * rh;
					float worst = 0.0f;
					for (size_t index = start; index <= end; ++index)
					{
						const float item_area = weights[order[index]] / rest * rw * rh;
						if (rw >= rh)
						{
							const float row_w = ImMax(row_area / rh, 0.0001f);
							const float item_h = ImMax(item_area / row_w, 0.0001f);
							worst = ImMax(worst, ImMax(row_w / item_h, item_h / row_w));
						}
						else
						{
							const float row_h = ImMax(row_area / rw, 0.0001f);
							const float item_w = ImMax(item_area / row_h, 0.0001f);
							worst = ImMax(worst, ImMax(item_w / row_h, row_h / item_w));
						}
					}
					return worst;
				};

			float rx = x;
			float ry = y;
			float rw = width;
			float rh = height;
			float rest = total;
			size_t start = 0;
			while (start < count && rest > 0.0f && rw > 0.5f && rh > 0.5f)
			{
				/* 行贪心：逐项试加、长宽比变差就封行 */
				size_t end = start;
				float row_sum = 0.0f;
				float best_worst = FLT_MAX;
				while (end < count && weights[order[end]] > 0.5f)
				{
					const float candidate_sum = row_sum + weights[order[end]];
					const float worst = worst_aspect(start, end, candidate_sum, rest, rw, rh);
					if (end > start && worst > best_worst)
						break;
					row_sum = candidate_sum;
					best_worst = worst;
					++end;
				}
				if (end == start)
					break;

				/* 铺行：末块补满剩余行程（吸收浮点误差，块之间不留缝） */
				const float row_area = row_sum / rest * rw * rh;
				if (rw >= rh)
				{
					const float row_w = row_area / rh;
					float cursor = ry;
					for (size_t index = start; index < end; ++index)
					{
						float item_h = weights[order[index]] / rest * rw * rh / row_w;
						if (index + 1 == end)
							item_h = ry + rh - cursor;
						out_pos[order[index]] = ImVec2(rx, cursor);
						out_size[order[index]] = ImVec2(row_w, item_h);
						cursor += item_h;
					}
					rx += row_w;
					rw -= row_w;
				}
				else
				{
					const float row_h = row_area / rw;
					float cursor = rx;
					for (size_t index = start; index < end; ++index)
					{
						float item_w = weights[order[index]] / rest * rw * rh / row_h;
						if (index + 1 == end)
							item_w = rx + rw - cursor;
						out_pos[order[index]] = ImVec2(cursor, ry);
						out_size[order[index]] = ImVec2(item_w, row_h);
						cursor += item_w;
					}
					ry += row_h;
					rh -= row_h;
				}

				rest -= row_sum;
				start = end;
			}
		}

		/* 三格滑动页签（CPU 卡的树 / Treemap / 火焰图 视图切换）：视觉与 SlidingTabPair
		 * 同一套（底槽 + 悬停提亮 + 选中块平滑滑动）；仅显示图标、名字走悬浮提示；
		 * 返回本帧被点击的格（-1 = 无）。width_override > 0 时用给定宽度
		 *（给行尾搜索框 / 按钮留位），否则占满内容区。 */
		int SlidingTabTriple(const char* id,
			const char* label0, Icons::Id icon0, const char* label1, Icons::Id icon1,
			const char* label2, Icons::Id icon2, int selected, float& slide, float width_override = 0.0f)
		{
			const ImGuiStyle& style = ImGui::GetStyle();
			const float height = ImGui::GetFrameHeight();
			const float width = width_override > 0.0f ? width_override : ImGui::GetContentRegionAvail().x;
			const float seg_width = width / 3.0f;
			const ImVec2 pos = ImGui::GetCursorScreenPos();

			int clicked = -1;
			bool hovered[3] = { false, false, false };
			ImGui::PushID(id);
			for (int index = 0; index < 3; ++index)
			{
				char segment_id[8];
				snprintf(segment_id, sizeof(segment_id), "##seg%d", index);
				ImGui::InvisibleButton(segment_id, ImVec2(seg_width, height));
				hovered[index] = ImGui::IsItemHovered();
				/* 仅显示图标：名字走悬浮提示 */
				if (hovered[index])
					ImGui::SetTooltip("%s", index == 0 ? label0 : (index == 1 ? label1 : label2));
				if (ImGui::IsItemClicked())
					clicked = index;
				if (index < 2)
					ImGui::SameLine(0.0f, 0.0f);
			}
			ImGui::PopID();

			/* 选中块的滑动：指数趋近（约 0.1s 滑到位），到目标即停避免长期微动 */
			const float target = static_cast<float>(selected);
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
			for (int index = 0; index < 3; ++index)
			{
				if (hovered[index] && index != selected)
				{
					const float seg_x = pos.x + seg_width * static_cast<float>(index);
					draw_list->AddRectFilled(ImVec2(seg_x, pos.y), ImVec2(seg_x + seg_width, pos.y + height),
						ImGui::GetColorU32(EditorTheme::Token::Neutral5), style.FrameRounding);
				}
			}

			/* 选中块（滑动中：位置在格间插值） */
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

			/* 格内容（仅图标，格内居中；名字走悬浮提示）：激活 / 悬停高亮 */
			const Icons::Id icons[3] = { icon0, icon1, icon2 };
			for (int index = 0; index < 3; ++index)
			{
				const float seg_x = pos.x + seg_width * static_cast<float>(index);
				const float icon_size = height * 0.62f;
				const ImU32 content_color = ImGui::GetColorU32(
					index == selected || hovered[index] ? EditorTheme::Token::Text : EditorTheme::Token::TextDim);
				Icons::Draw(draw_list, icons[index],
					ImVec2(seg_x + seg_width * 0.5f, pos.y + height * 0.5f), icon_size, content_color);
			}

			return clicked;
		}
	}

	void RenderStatsPanel::OnImGuiRender()
	{
		PROFILE_FUNCTION();

		/* 追踪文本与录制同起同停（状态驱动、每帧一次；面板收起 / 换视图也照常结算） */
		SyncCPUTraceTextSession();

		ImGui::Begin(Panel::kStatInfo);
		{
			/* 面板标题由页签承担（图标 + "Stat Info"），面板内不再重复一行 */

			ShowFrameStatsCard();
			ShowCPUTimingsCard();
			ShowGPUTimingsCard();
		}
		ImGui::End();

		/* 图表放大窗口：独立顶层窗口，必须画在面板窗口之外 */
		ShowCPUChartWindow();
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

			ShowFrameTimeGraph(Scaled(46.0f));

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

			DrawBrokenSeries(draw_list, count,
				[](uint32_t) { return true; },
				[&](uint32_t index) { return ImVec2(column_x(index), value_y(profiler.GetHistorySample(index).FrameMs)); },
				ImGui::GetColorU32(kFrameLineColor), Scaled(1.2f));

			DrawBrokenSeries(draw_list, count,
				[](uint32_t) { return true; },
				[&](uint32_t index) { return ImVec2(column_x(index), value_y(profiler.GetHistorySample(index).CpuMs)); },
				ImGui::GetColorU32(kCpuLineColor), Scaled(1.2f));

			DrawBrokenSeries(draw_list, count,
				[&](uint32_t index) { return profiler.GetHistorySample(index).GpuValid; },
				[&](uint32_t index) { return ImVec2(column_x(index), value_y(profiler.GetHistorySample(index).GpuMs)); },
				ImGui::GetColorU32(kGpuLineColor), Scaled(1.2f));

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

		legend_item("Frame", kFrameLineColor, true);
		legend_item("CPU", kCpuLineColor, false);
		legend_item("GPU", kGpuLineColor, false);
	}

	/* CPU 计时卡：函数级耗时卡（树 / Treemap / 火焰图三视图；对偶 GPU Timings 卡）。
	 * 数据源 = CPUTimerProfiler（帧内按「父 + 标签」聚合，单位微秒）；Trace Text 开关在录制
	 * 工具栏首钮（写盘会话与录制同起同停，见 SyncCPUTraceTextSession）。 */
	void RenderStatsPanel::ShowCPUTimingsCard()
	{
		PROFILE_FUNCTION();

		const PanelChrome::Card card = PanelChrome::BeginCard("CPU Timings", Icons::Id::Stats);

		if (card.Open)
		{
			/* 两张计时卡同处一个面板窗口：卡内控件各套一层 ID 作用域，
			 * 同图标的按钮才不会撞 id（撞了后面那个点不动） */
			ImGui::PushID("CPU Timings");

			bool timer_enabled = CPUTimerProfiler::Instance().IsEnabled();
			if (ImGuiExt::DrawCheckboxUI("CPU Timer", timer_enabled))
			{
				CPUTimerProfiler::Instance().SetEnabled(timer_enabled);
				/* 计时关闭：收回录制（已录内容保留，重开后可继续回放） */
				if (!timer_enabled)
					CPUTimerProfiler::Instance().GetRecorder().Stop();
			}

			CPUTimerRecorder& recorder = CPUTimerProfiler::Instance().GetRecorder();

			if (timer_enabled)
			{
				const int switched = SlidingTabPair("##CPUViewTabs", "Live", Icons::Id::Live,
					"Record", Icons::Id::Record, m_CpuRecordView, m_CpuTabSlide);
				if (switched == 0)
					m_CpuRecordView = false;
				else if (switched == 1 && !m_CpuRecordView)
				{
					m_CpuRecordView = true;
					if (!recorder.IsRecording() && recorder.GetFrameCount() > 0)
						m_CpuReplayFrame = static_cast<int>(recorder.GetFrameCount()) - 1;
				}
			}

			/* 录制工具栏（首钮 = 追踪文本开关）：录制视图常显；实时视图只在录制进行中显示 */
			if (timer_enabled && (m_CpuRecordView || recorder.IsRecording()))
				ShowCPUTimerToolbar();

			/* 录制进度：帧数 + 细进度条（红 = 录制中） */
			if (timer_enabled && recorder.IsRecording())
			{
				const uint32_t count_now = recorder.GetFrameCount();
				ImGui::TextDisabled("Recording %u / %u", count_now, CPUTimerRecorder::MAX_FRAMES);

				const float progress = static_cast<float>(count_now)
					/ static_cast<float>(CPUTimerRecorder::MAX_FRAMES);
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
			if (m_CpuReplayFrame >= static_cast<int>(recorded_count))
				m_CpuReplayFrame = -1;

			/* 录制刚结束（手动 / 录满 / 计时关闭）且在看录制视图：停在最后一帧 */
			if (m_CpuRecordView && m_CpuWasRecording && !recorder.IsRecording() && recorded_count > 0)
				m_CpuReplayFrame = static_cast<int>(recorded_count) - 1;
			m_CpuWasRecording = recorder.IsRecording();

			if (m_CpuRecordView && !recorder.IsRecording() && recorded_count > 0 && m_CpuReplayFrame < 0)
				m_CpuReplayFrame = static_cast<int>(recorded_count) - 1;

			const bool replaying = timer_enabled && m_CpuRecordView && !recorder.IsRecording()
				&& recorded_count > 0 && m_CpuReplayFrame >= 0;
			if (replaying)
				ShowCPUTimerRecordingUI();

			/* 显示根：录制视图 = 选中录制帧；其余 = 实时树（EMA）——与放大窗口共用 */
			const ResultCPUTimerNode* display_root = ResolveDisplayRoot();

			if (!timer_enabled)
			{
				ImGuiExt::DrawCommonTextUI("Timers", "Off");
			}
			else if (m_CpuRecordView && !replaying)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
				if (recorder.IsRecording())
					ImGui::TextWrapped("Frame timings will appear here when recording finishes.");
				else
					ImGui::TextWrapped("Press Record to capture up to %u frames of per-function CPU timings. "
						"Recording stops when full, or press Stop at any time. When finished, browse "
						"frames above and inspect each frame's timings here.", CPUTimerRecorder::MAX_FRAMES);
				ImGui::PopStyleColor();
			}
			else if (display_root == nullptr || display_root->Label.empty())
			{
				ImGuiExt::DrawCommonTextUI("Timers", "No data");
			}
			else
			{
				/* 图表视图切换（紧凑宽度、仅图标，悬浮出名字）+ 行内搜索（右对齐，吃余量）
				 * + 行尾 [抓取当前帧（实时才出现）][在窗口中打开] */
				const ImGuiStyle& row_style = ImGui::GetStyle();
				const float row_width = ImGui::GetContentRegionAvail().x;
				const float pop_out_size = ImGui::GetFrameHeight();
				/* 抓取钮位：录制视图不出现（可抓的实时流只在实时视图里） */
				const float capture_size = pop_out_size;
				const float capture_reserve = !m_CpuRecordView
					? capture_size + row_style.ItemSpacing.x : 0.0f;
				const float switcher_width = ImMin(TimingSwitcherWidth(),
					ImMax(row_width - 70.0f - pop_out_size - capture_reserve
						- row_style.ItemSpacing.x * 3.0f, 0.0f));
				const float search_width = TimingSearchWidth(row_width, switcher_width,
					pop_out_size + capture_reserve);
				const int switched_view = SlidingTabTriple("##CPUChartTabs",
					"Tree", Icons::Id::Menu, "Treemap", Icons::Id::Grid, "Flame", Icons::Id::Stats,
					static_cast<int>(m_CpuChartView), m_CpuChartSlide, switcher_width);
				if (switched_view >= 0)
					m_CpuChartView = static_cast<CpuChartView>(switched_view);

				SameLineAt(row_width - search_width - pop_out_size - capture_reserve - row_style.ItemSpacing.x);
				TimingSearchInput("##CPUTimingSearch", m_CpuTimingSearch,
					IM_ARRAYSIZE(m_CpuTimingSearch), m_CpuTimingSearchFilter,
					m_CpuTimingSearchMatchCount, search_width);

				/* 抓取当前帧（搜索右侧、实时视图）：冻结 = 显示根改用快照供逐行分析；再点解冻 */
				if (!m_CpuRecordView)
				{
					SameLineAt(row_width - pop_out_size - capture_size - row_style.ItemSpacing.x);
					if (Icons::IconButton(Icons::Id::Capture, ImVec2(capture_size, capture_size), m_CpuFrozen,
						m_CpuFrozen ? "Resume live updates" : "Capture current frame (freeze for inspection)"))
						ToggleCPUCaptureFrame();
				}

				SameLineAt(row_width - pop_out_size);
				if (Icons::IconButton(Icons::Id::FitView, ImVec2(pop_out_size, pop_out_size),
					m_CpuChartWindowOpen, "Open in a window"))
					m_CpuChartWindowOpen = !m_CpuChartWindowOpen;

				if (m_CpuChartWindowOpen)
				{
					/* 弹窗开着：面板里的图表视图让位（同一份数据只同时显示一个）——行留在原处，
					 * 行尾按钮 / 弹窗标题栏 X 都能把视图收回来 */
					ImGuiExt::DrawCommonTextUI("Timers", "Shown in the CPU Chart window");
				}
				else
				{
					const float chart_width = ImGui::GetContentRegionAvail().x;
					switch (m_CpuChartView)
					{
					case CpuChartView::Treemap:
						ShowCPUTreemapView(*display_root, chart_width, 180.0f, false);
						break;
					case CpuChartView::Flame:
						ShowCPUFlameView(*display_root, chart_width, 150.0f);
						break;
					case CpuChartView::Tree:
					default:
						ShowCPUTreeView(*display_root);
						break;
					}
				}
			}

			ImGui::PopID();
		}

		PanelChrome::EndCard(card);
	}

	/* CPU 卡的树表视图：函数级结果树（整行可选中；浅层默认展开） */
	void RenderStatsPanel::ShowCPUTreeView(const ResultCPUTimerNode& root)
	{
		PROFILE_FUNCTION();

		/* 命中计数每帧随树结算（搜索框在卡片行、悬浮提示用；自检可读） */
		m_CpuTimingSearchMatchCount = TimingMatchCount(root, m_CpuTimingSearchFilter);
		const bool filtering = !m_CpuTimingSearchFilter.empty();

		/* 行 id 用遍历序号（结构稳定时跨帧一致 —— 折叠状态可保持） */
		uint32_t node_id = 0;
		std::function<void(const ResultCPUTimerNode&, uint32_t)> show_node_recursively;
		show_node_recursively = [&show_node_recursively, &node_id, filtering, this](
			const ResultCPUTimerNode& timer_node, uint32_t depth)
			{
				if (timer_node.Label.empty())
					return;

				/* 搜索过滤：自身与后代都无命中的整支不画（过滤清空时恒可见） */
				if (filtering && !TimingSubtreeMatches(timer_node, m_CpuTimingSearchFilter))
					return;

				const uint32_t this_id = node_id++;
				const bool has_children = !timer_node.Children.empty();

				/* 只展开浅层，深层手动；点行任意一处（含箭头那一下）都选中这行；
				 * 过滤中：命中路径上的祖父行强制展开 */
				const TimingRowState row = TimingTreeRow(
					reinterpret_cast<void*>(static_cast<intptr_t>(this_id)),
					timer_node.Label.c_str(), has_children, depth < 2,
					m_CpuSelectedNode == static_cast<int>(this_id),
					filtering && !TimingNodeMatches(timer_node, m_CpuTimingSearchFilter));
				if (row.Clicked)
					m_CpuSelectedNode = static_cast<int>(this_id);

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
					TimingLocationText(timer_node);
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

	/* Treemap 布局（squarified）：每层 =「自身 self 块 + 各子节点块」（权重 = 耗时常量），
	 * 面积与耗时成正比；父块先序输出、子块嵌套其中（先序 + 后画覆盖 = 命中自然偏最深）。
	 * 纯自身节点（无子节点 / 子耗时趋零）不铺 self 块 —— 节点块本身就是它。 */
	void RenderStatsPanel::LayoutTreemap(const ResultCPUTimerNode& root, float width, float height,
		float inset, std::vector<TreemapBlock>& out_blocks)
	{
		out_blocks.clear();

		std::function<void(const ResultCPUTimerNode&, float, float, float, float, int32_t, uint32_t)> layout_node;
		layout_node = [&layout_node, &out_blocks, inset](
			const ResultCPUTimerNode& node, float x, float y, float w, float h,
			int32_t parent_block, uint32_t depth)
			{
				if (w < 1.0f || h < 1.0f)
					return;

				TreemapBlock block{};
				block.Node = &node;
				block.X = x;
				block.Y = y;
				block.Width = w;
				block.Height = h;
				block.ParentBlock = parent_block;
				const int32_t block_index = static_cast<int32_t>(out_blocks.size());
				out_blocks.emplace_back(block);

				/* 内缩区域：太深 / 太小不再细分（防病理树） */
				if (depth >= 10)
					return;
				const float inner_x = x + inset;
				const float inner_y = y + inset;
				const float inner_w = w - inset * 2.0f;
				const float inner_h = h - inset * 2.0f;
				if (inner_w < 4.0f || inner_h < 4.0f)
					return;

				/* 权重表：[0] = 自身 self（无子节点时不铺 —— 节点块本身就是它）+ 子节点 Total */
				const bool has_children = !node.Children.empty();
				std::vector<float> weights;
				weights.reserve(node.Children.size() + 1);
				weights.emplace_back(has_children ? static_cast<float>(node.SelfUs) : 0.0f);
				double children_total = 0.0;
				for (const auto& child : node.Children)
				{
					weights.emplace_back(static_cast<float>(child.TotalUs));
					children_total += child.TotalUs;
				}

				std::vector<ImVec2> item_pos;
				std::vector<ImVec2> item_size;
				SquarifyInto(weights, inner_x, inner_y, inner_w, inner_h, item_pos, item_size);

				/* self 块：有子节点、自身与子耗时都够量才铺 —— 纯自身节点（子耗时趋零）不
				 * 单独铺（节点块本身就是它，不然会多一张同名卡） */
				if (has_children && weights[0] > 0.5f && children_total > 0.5
					&& item_size[0].x > 0.5f && item_size[0].y > 0.5f)
				{
					TreemapBlock self_block{};
					self_block.Node = &node;
					self_block.IsSelf = true;
					self_block.X = item_pos[0].x;
					self_block.Y = item_pos[0].y;
					self_block.Width = item_size[0].x;
					self_block.Height = item_size[0].y;
					self_block.ParentBlock = block_index;
					out_blocks.emplace_back(self_block);
				}

				for (size_t index = 0; index < node.Children.size(); ++index)
				{
					const size_t slot = index + 1;
					if (item_size[slot].x <= 0.5f || item_size[slot].y <= 0.5f)
						continue;
					layout_node(node.Children[index], item_pos[slot].x, item_pos[slot].y,
						item_size[slot].x, item_size[slot].y, block_index, depth + 1);
				}
			};

		layout_node(root, 0.0f, 0.0f, width, height, -1, 0);
	}

	/* 火焰图布局（时间序嵌套；纯函数，headless 可断言）：子块按起始时间排、宽度按总耗时占比
	 * 铺满父块（父子直接相连、收起不重排）；纵向一行一深度。折叠命中的节点「本块保留、
	 * 不再下钻」；显示根不折。 */
	void RenderStatsPanel::LayoutFlame(const ResultCPUTimerNode& root,
		const std::unordered_set<std::string>* collapsed, const std::string& root_path,
		std::vector<FlameBlock>& out_blocks)
	{
		out_blocks.clear();

		std::function<void(const ResultCPUTimerNode&, const std::string&, uint32_t, double, double, int32_t)> collect;
		collect = [&collect, &out_blocks, collapsed](
			const ResultCPUTimerNode& node, const std::string& path, uint32_t depth,
			double x0, double x1, int32_t parent_block)
			{
				if (node.Label.empty())
					return;

				FlameBlock block{};
				block.Node = &node;
				block.Depth = depth;
				block.X0 = x0;
				block.X1 = x1;
				block.ParentBlock = parent_block;
				block.Collapsed = collapsed != nullptr && depth > 0 && collapsed->count(path) > 0;
				const int32_t block_index = static_cast<int32_t>(out_blocks.size());
				out_blocks.emplace_back(block);

				/* 收起：子树不再下钻（本块照留、位置不变） */
				if (block.Collapsed)
					return;

				/* 子块：按起始时间排序；宽度 = 总耗时占比（归一化铺满父块，末块补满吸收
				 * 浮点误差）；零耗时 / 空名的子块不铺 */
				std::vector<const ResultCPUTimerNode*> ordered;
				ordered.reserve(node.Children.size());
				double children_total = 0.0;
				for (const auto& child : node.Children)
				{
					if (child.Label.empty() || child.TotalUs <= 0.0)
						continue;
					children_total += child.TotalUs;
					ordered.push_back(&child);
				}
				if (children_total <= 0.0)
					return;
				std::stable_sort(ordered.begin(), ordered.end(),
					[](const ResultCPUTimerNode* left, const ResultCPUTimerNode* right)
						{ return left->BeginUs < right->BeginUs; });

				const double span = x1 - x0;
				double cursor = x0;
				for (size_t index = 0; index < ordered.size(); ++index)
				{
					const ResultCPUTimerNode& child = *ordered[index];
					double child_width = span * (child.TotalUs / children_total);
					if (index + 1 == ordered.size())
						child_width = x1 - cursor;   /* 末块补满（吸收浮点误差） */
					const std::string child_path = path.empty() ? child.Label : path + "/" + child.Label;
					collect(child, child_path, depth + 1, cursor, cursor + child_width, block_index);
					cursor += child_width;
				}
			};
		collect(root, root_path, 0, 0.0, 1.0, -1);
	}

	/* CPU 卡的 Treemap 视图：面积 = 耗时占比（叶层全是 self 块、铺满）、颜色 = 热力；
	 * 悬停读数、点击下钻（面包屑返回）。下钻路径按 label 链对位 —— 换帧后失效段自动截断。
	 * show_duration = 条目标签在名字下附一行耗时（弹窗 / 最大化用 —— 地方大；卡片保持
	 * 名字-only，块小不塞数字）。 */
	void RenderStatsPanel::ShowCPUTreemapView(const ResultCPUTimerNode& root, float width, float height,
		bool show_duration)
	{
		PROFILE_FUNCTION();

		/* 下钻路径解析（按 label 链；换帧 / 换数据源后截断失效段） */
		const ResultCPUTimerNode* zoom_root = &root;
		{
			size_t valid = 0;
			const ResultCPUTimerNode* node = &root;
			for (; valid < m_CpuTreemapZoom.size(); ++valid)
			{
				const ResultCPUTimerNode* next = nullptr;
				for (const auto& child : node->Children)
				{
					if (child.Label == m_CpuTreemapZoom[valid])
					{
						next = &child;
						break;
					}
				}
				if (next == nullptr)
					break;
				node = next;
			}
			if (valid < m_CpuTreemapZoom.size())
				m_CpuTreemapZoom.resize(valid);
			zoom_root = node;
		}

		/* 面包屑（下钻中）：一键回根 + 返回上级 + 路径（逐级可点、超框自动换行 ——
		 * 点第 k 级直接跳回那一层；末级亮显、祖先暗显，" / " 作分隔、换行处省略分隔符） */
		if (!m_CpuTreemapZoom.empty())
		{
			const float back_size = ImGui::GetFrameHeight();
			/* 换行右界（屏幕坐标）：行首 + 整行可用宽（本行起点处的剩余区域宽） */
			const float wrap_right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
			/* 一键回根（清空整条下钻路径；逐层返回之外的快捷方式） */
			if (Icons::IconButton(Icons::Id::ReturnRoot, ImVec2(back_size, back_size), false, "Back to root"))
				m_CpuTreemapZoom.clear();
			ImGui::SameLine();
			if (Icons::IconButton(Icons::Id::Return, ImVec2(back_size, back_size), false, "Back to parent"))
				m_CpuTreemapZoom.pop_back();

			/* 路径逐级可点 + 超框换行：每段 = 与图标钮同尺寸（同高）的 InvisibleButton（命中 /
			 * 悬停 / 点击），悬停底与文字用 draw_list 按几何中线画 —— Selectable 的基线偏移会被
			 * 下一段 SameLine 继承而漂移，不用。 */
			ImDrawList* crumb_draw_list = ImGui::GetWindowDrawList();
			int jump_to = -1;
			for (size_t index = 0; index < m_CpuTreemapZoom.size(); ++index)
			{
				const std::string& label = m_CpuTreemapZoom[index];
				const float spacing = ImGui::GetStyle().ItemSpacing.x;
				const float label_text_width = ImGui::CalcTextSize(label.c_str()).x;
				const float sep_width = ImGui::CalcTextSize("/").x;
				const float text_y_offset = (back_size - ImGui::GetTextLineHeight()) * 0.5f;

				if (index == 0)
				{
					ImGui::SameLine();
				}
				else if (ImGui::GetItemRectMax().x + sep_width + spacing * 2.0f + label_text_width <= wrap_right)
				{
					/* 行内继续：分隔符 + 段 */
					ImGui::SameLine();
					ImGui::PushID(static_cast<int>(index));
					ImGui::InvisibleButton("##crumb_sep", ImVec2(sep_width, back_size));
					const ImVec2 sep_min = ImGui::GetItemRectMin();
					crumb_draw_list->AddText(ImVec2(sep_min.x, sep_min.y + text_y_offset),
						ImGui::GetColorU32(EditorTheme::Token::TextDim), "/");
					ImGui::PopID();
					ImGui::SameLine();
				}
				else
				{
					/* 超框换行：不 SameLine、省略分隔符（行首自然承接），本段落在新行 */
				}

				ImGui::PushID(static_cast<int>(index));
				ImGui::InvisibleButton("##crumb_seg", ImVec2(label_text_width, back_size));
				const ImVec2 seg_min = ImGui::GetItemRectMin();
				const bool seg_hovered = ImGui::IsItemHovered();
				if (seg_hovered)
					crumb_draw_list->AddRectFilled(seg_min,
						ImVec2(seg_min.x + label_text_width, seg_min.y + back_size),
						ImGui::GetColorU32(EditorTheme::Token::Neutral5), 3.0f);
				crumb_draw_list->AddText(ImVec2(seg_min.x, seg_min.y + text_y_offset),
					ImGui::GetColorU32(index + 1 == m_CpuTreemapZoom.size()
						? EditorTheme::Token::Text : EditorTheme::Token::TextDim), label.c_str());
				if (ImGui::IsItemClicked())
					jump_to = static_cast<int>(index);
				if (seg_hovered)
					ImGui::SetTooltip("Jump to this level");
				ImGui::PopID();
			}
			if (jump_to >= 0)
				m_CpuTreemapZoom.resize(static_cast<size_t>(jump_to) + 1);
		}

		const ImVec2 canvas_min = ImGui::GetCursorScreenPos();
		const ImVec2 canvas_max(canvas_min.x + width, canvas_min.y + height);
		ImGui::InvisibleButton("##CPUTreemapCanvas", ImVec2(width, height));
		const bool hovered = ImGui::IsItemHovered();
		const bool clicked = ImGui::IsItemClicked();

		std::vector<TreemapBlock> blocks;
		LayoutTreemap(*zoom_root, width, height, 2.0f, blocks);

		ImDrawList* draw_list = ImGui::GetWindowDrawList();
		draw_list->AddRectFilled(canvas_min, canvas_max, ImGui::GetColorU32(EditorTheme::Token::Neutral1), 3.0f);

		const ImVec2 mouse = ImGui::GetIO().MousePos;
		int hover_block = -1;

		for (size_t index = 0; index < blocks.size(); ++index)
		{
			const TreemapBlock& block = blocks[index];
			const ImVec2 min(canvas_min.x + block.X, canvas_min.y + block.Y);
			const ImVec2 max(min.x + block.Width, min.y + block.Height);

			const double value = block.IsSelf ? block.Node->SelfUs : block.Node->TotalUs;
			draw_list->AddRectFilled(min, max, HeatFillForUs(value), 1.5f);
			draw_list->AddRect(min, max, ImGui::GetColorU32(EditorTheme::Token::Neutral0), 1.5f, 0, 1.0f);

			/* 标签：块内自动回车（按块宽折行）—— 垂直只放整行（不切半个字），窄到连一行
			 * 都放不下就不画。真裁剪必须走 draw_list 的 clip：RenderText 的 clip_rect 只做
			 * 粗剔除（整字形在框外才跳过），跨边界的字形会照画出去。 */
			const float line_height = ImGui::GetTextLineHeight();
			const int max_lines = static_cast<int>((block.Height - 4.0f) / line_height);
			if (block.Width >= 34.0f && max_lines >= 1)
			{
				/* 标签只写函数名（self 块不再带 " self" 后缀 —— 悬停读数里有 "(self)"） */
				char label[160];
				if (show_duration)
				{
					char time_text[32];
					FormatBlockTime(value, time_text, sizeof(time_text));
					snprintf(label, sizeof(label), "%s\n%s", block.Node->Label.c_str(), time_text);
				}
				else
				{
					snprintf(label, sizeof(label), "%s", block.Node->Label.c_str());
				}

				const ImVec2 text_pos(min.x + 4.0f, min.y + 2.0f);
				const ImVec2 clip_max(max.x, ImMin(max.y, text_pos.y + static_cast<float>(max_lines) * line_height));
				draw_list->PushClipRect(min, clip_max, true);
				ImGui::GetFont()->RenderText(draw_list, ImGui::GetFontSize(), text_pos, HeatTextForUs(value),
					ImVec4(min.x, min.y, clip_max.x, clip_max.y), label, nullptr, block.Width - 8.0f, false);
				draw_list->PopClipRect();
			}

			/* 命中（正序后覆盖 = 更深的块优先） */
			if (hovered && mouse.x >= min.x && mouse.x < max.x && mouse.y >= min.y && mouse.y < max.y)
				hover_block = static_cast<int>(index);
		}

		/* 悬停：高亮描边 + 读数 */
		if (hover_block >= 0)
		{
			const TreemapBlock& block = blocks[hover_block];
			const ImVec2 min(canvas_min.x + block.X, canvas_min.y + block.Y);
			const ImVec2 max(min.x + block.Width, min.y + block.Height);
			draw_list->AddRect(min, max, ImGui::GetColorU32(EditorTheme::Token::AccentHover), 1.5f, 0, 2.0f);

			const ResultCPUTimerNode& node = *block.Node;
			const double view_total = zoom_root->TotalUs > 0.0 ? zoom_root->TotalUs : 1.0;
			const double share = block.IsSelf ? node.SelfUs : node.TotalUs;
			ImGui::BeginTooltip();
			ImGui::Text("%s%s", node.Label.c_str(), block.IsSelf ? " (self)" : "");
			TimingLocationText(node);
			ImGui::Text("total %.2f us   self %.2f us", node.TotalUs, node.SelfUs);
			ImGui::Text("%.1f%% of view · %u calls", share / view_total * 100.0, node.CallCount);
			if (!node.Children.empty())
				ImGui::TextDisabled("Click to zoom in");
			ImGui::EndTooltip();
		}

		/* 点击下钻：任何节点都可点进去（含叶子 —— 叶子成为独立视图：单块铺满 + 路径延伸），
		 * 路径 =「显示根 → 命中节点」的 label 链。例外 = 显示根本身（或其 self 块）：空操作
		 *（否则路径尾追加重复段，下一帧解析失败又自截断） */
		if (clicked && hover_block >= 0)
		{
			const TreemapBlock& block = blocks[hover_block];
			if (block.Node != nullptr && block.Node != zoom_root)
			{
				std::vector<std::string> chain;
				for (int32_t cursor = hover_block; cursor >= 0; cursor = blocks[cursor].ParentBlock)
				{
					if (!blocks[cursor].IsSelf)
						chain.emplace_back(blocks[cursor].Node->Label);
				}
				/* chain = [目标 ... 显示根]（逆序）—— 去掉显示根再翻正接上 */
				for (size_t index = chain.size(); index-- > 1;)
					m_CpuTreemapZoom.emplace_back(chain[index - 1]);
			}
		}
	}

	/* 火焰图（时间序嵌套）：横向 = 子块按起始时间排、宽度按耗时占比铺满父块（父子直接相连、
	 * 收起不重排），纵向 = 调用深度（帧根在最上）。条目上标耗时；悬停读数；单击收起 / 展开；
	 * 滚轮缩放（锚在光标）、拖拽平移、右键复位（FrameGraph 同一套手感）；缩放后右下角浮出
	 * 缩略图（可拖 / 点跳）；悬停画全高竖线。 */
	void RenderStatsPanel::ShowCPUFlameView(const ResultCPUTimerNode& root, float width, float height)
	{
		PROFILE_FUNCTION();

		/* 布局（时间序嵌套；收起键 = 显示根起的绝对 label 链） */
		std::vector<FlameBlock> blocks;
		LayoutFlame(root, &m_CpuFlameCollapsed, root.Label, blocks);

		/* 画布 + 视图窗口映射（归一化 x → 画布 x；缩放 / 平移见下） */
		const ImVec2 canvas_min = ImGui::GetCursorScreenPos();
		const ImVec2 canvas_max(canvas_min.x + width, canvas_min.y + height);
		ImGui::InvisibleButton("##CPUFlameCanvas", ImVec2(width, height));
		const bool hovered = ImGui::IsItemHovered();
		const ImGuiIO& io = ImGui::GetIO();

		ImDrawList* draw_list = ImGui::GetWindowDrawList();
		draw_list->AddRectFilled(canvas_min, canvas_max, ImGui::GetColorU32(EditorTheme::Token::Neutral1), 3.0f);

		const float plot_width = ImMax(width - 4.0f, 1.0f);
		const double view_w = 1.0 / static_cast<double>(m_CpuFlameZoom);
		const auto view_clamp = [this, view_w]()
			{
				m_CpuFlamePan = static_cast<float>(
					ImClamp(static_cast<double>(m_CpuFlamePan), 0.0, 1.0 - view_w));
			};
		const auto time_x = [&](double x01)
			{
				return canvas_min.x + 2.0f
					+ static_cast<float>((x01 - static_cast<double>(m_CpuFlamePan)) / view_w) * plot_width;
			};
		/* 右下角缩略图几何（仅缩放超出画布时显示；小浮块，不撑满底栏）：
		 * 微型火焰缩画 + 高亮框标当前视图窗口位置 */
		const bool show_mini = m_CpuFlameZoom > 1.0f;
		const float mini_w = ImMin(plot_width * 0.30f, 260.0f);
		const float mini_h = ImMin(64.0f, height * 0.32f);
		const float mini_x1 = canvas_max.x - 8.0f;
		const float mini_y1 = canvas_max.y - 8.0f;
		const float mini_x0 = mini_x1 - mini_w;
		const float mini_y0 = mini_y1 - mini_h;
		const bool mouse_in_mini = show_mini && hovered
			&& io.MousePos.x >= mini_x0 && io.MousePos.y >= mini_y0;
		/* 缩略图内的视图窗口高亮几何（x0, 宽）：位置 = pan 在整段数据里的比例；最小 12px 可抓 */
		const auto mini_thumb = [&]() -> ImVec2
			{
				float thumb_x0 = mini_x0 + static_cast<float>(m_CpuFlamePan) * mini_w;
				float thumb_w = static_cast<float>(view_w) * mini_w;
				if (thumb_w < 12.0f)
					thumb_w = 12.0f;
				return ImVec2(thumb_x0, thumb_w);
			};

		/* 交互（与 FrameGraph 面板同一套）：滚轮缩放锚在光标；拖拽平移（位移过阈值才算拖，
		 * 未过阈值松开 = 单击）；右键复位。SetItemUsingMouseWheel：光标在画布上时吃掉滚轮，
		 * 不让面板窗口跟着滚（imgui 按"上一帧的 hover 标记"拦滚动，所以 hover 帧帧登记）。 */
		if (hovered)
			ImGui::SetItemUsingMouseWheel();
		if (hovered && io.MouseWheel != 0.0f)
		{
			const float new_zoom = static_cast<float>(ImClamp(
				static_cast<double>(m_CpuFlameZoom) * std::pow(1.15, static_cast<double>(io.MouseWheel)),
				1.0, 64.0));
			if (new_zoom != m_CpuFlameZoom)
			{
				const float anchor_ratio = ImClamp((io.MousePos.x - (canvas_min.x + 2.0f)) / plot_width, 0.0f, 1.0f);
				const double anchor = static_cast<double>(m_CpuFlamePan)
					+ static_cast<double>(anchor_ratio) * view_w;
				m_CpuFlameZoom = new_zoom;
				m_CpuFlamePan = static_cast<float>(
					anchor - static_cast<double>(anchor_ratio) / static_cast<double>(m_CpuFlameZoom));
				view_clamp();
			}
		}
		if (ImGui::IsItemActivated())
		{
			m_CpuFlameDraggedDistance = 0.0f;
			m_CpuFlameScrollGrab = mouse_in_mini;
			if (m_CpuFlameScrollGrab)
			{
				/* 点缩略图（高亮框之外）= 跳到该处：视图中心对准点击位置 */
				const ImVec2 thumb = mini_thumb();
				const bool in_thumb = io.MousePos.x >= thumb.x && io.MousePos.x <= thumb.x + thumb.y;
				if (!in_thumb)
				{
					const float track_frac = ImClamp((io.MousePos.x - mini_x0) / mini_w, 0.0f, 1.0f);
					m_CpuFlamePan = static_cast<float>(
						static_cast<double>(track_frac) - view_w * 0.5);
					view_clamp();
				}
			}
		}
		if (ImGui::IsItemActive())
		{
			if (m_CpuFlameScrollGrab)
			{
				/* 拖高亮框：把手跟手（缩略图像素 → 归一化） */
				m_CpuFlamePan = static_cast<float>(static_cast<double>(m_CpuFlamePan)
					+ io.MouseDelta.x / mini_w);
				view_clamp();
			}
			else
			{
				m_CpuFlameDraggedDistance += ImFabs(io.MouseDelta.x);
				if (m_CpuFlameDraggedDistance > 3.0f)
				{
					m_CpuFlamePan = static_cast<float>(static_cast<double>(m_CpuFlamePan)
						- io.MouseDelta.x / plot_width * view_w);
					view_clamp();
				}
			}
		}
		if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		{
			m_CpuFlameZoom = 1.0f;
			m_CpuFlamePan = 0.0f;
		}

		/* 行高布局：一行一深度（装不下的深度不画） */
		constexpr float kRowHeight = 15.0f;
		constexpr float kRowGap = 1.0f;

		const FlameBlock* hover_block = nullptr;
		ImVec2 hover_min{ 0.0f, 0.0f };
		ImVec2 hover_max{ 0.0f, 0.0f };

		for (const FlameBlock& block : blocks)
		{
			const float row_y = canvas_min.y + 2.0f
				+ static_cast<float>(block.Depth) * (kRowHeight + kRowGap);
			if (row_y + kRowHeight > canvas_max.y)
				continue;

			float x0 = time_x(block.X0);
			float x1 = time_x(block.X1);
			if (x1 <= canvas_min.x || x0 >= canvas_max.x)
				continue;   /* 滚出视图窗口的块不画（也不参与命中） */
			if (x1 - x0 < 1.0f)
				x1 = x0 + 1.0f;
			const ImVec2 min(x0, row_y);
			const ImVec2 max(x1, row_y + kRowHeight);

			draw_list->AddRectFilled(min, max, HeatFillForUs(block.Node->TotalUs), 1.5f);
			draw_list->AddRect(min, max, ImGui::GetColorU32(EditorTheme::Token::Neutral0), 1.5f, 0, 1.0f);

			/* 标签：名字左对齐；耗时右对齐（收起中的条目给「＋」让位；两者相撞就不画耗时） */
			if (max.x - min.x >= 44.0f)
			{
				draw_list->PushClipRect(min, max, true);
				draw_list->AddText(ImVec2(min.x + 4.0f, min.y + 1.0f),
					HeatTextForUs(block.Node->TotalUs), block.Node->Label.c_str());
				draw_list->PopClipRect();
			}
			if (max.x - min.x >= 72.0f)
			{
				char time_text[32];
				FormatBlockTime(block.Node->TotalUs, time_text, sizeof(time_text));
				const float time_width = ImGui::CalcTextSize(time_text).x;
				const float name_width = ImGui::CalcTextSize(block.Node->Label.c_str()).x;
				const float right_pad = block.Collapsed ? 16.0f : 4.0f;
				if (min.x + 4.0f + name_width + 10.0f < max.x - right_pad - time_width)
				{
					draw_list->AddText(ImVec2(max.x - right_pad - time_width, min.y + 1.0f),
						HeatTextForUs(block.Node->TotalUs), time_text);
				}
			}

			/* 收起中的条目：右端一个小「＋」（点条目 = 展开） */
			if (block.Collapsed)
			{
				const ImU32 fold_color = HeatTextForUs(block.Node->TotalUs);
				const float fold_x = max.x - 6.0f;
				const float fold_y = min.y + kRowHeight * 0.5f;
				draw_list->AddLine(ImVec2(fold_x - 3.0f, fold_y), ImVec2(fold_x + 3.0f, fold_y), fold_color, 1.4f);
				draw_list->AddLine(ImVec2(fold_x, fold_y - 3.0f), ImVec2(fold_x, fold_y + 3.0f), fold_color, 1.4f);
			}

			if (hovered && io.MousePos.x >= min.x && io.MousePos.x < max.x
				&& io.MousePos.y >= min.y && io.MousePos.y < max.y)
			{
				hover_block = &block;
				hover_min = min;
				hover_max = max;
			}
		}

		/* 鼠标竖线：指向位置参考（hover 时全高一根细线） */
		if (hovered)
		{
			draw_list->AddLine(ImVec2(io.MousePos.x, canvas_min.y + 2.0f),
				ImVec2(io.MousePos.x, canvas_max.y - 2.0f),
				ImGui::GetColorU32(EditorTheme::Token::TextLabel), 1.0f);
		}

		/* 右下角缩略图（仅缩放超出画布时）：整幅火焰按深度缩画（同热量配色）+ 当前窗口
		 * 区间高亮（半透明 accent 底 + 亮边；拖它 / 点缩略图滑 —— 命中逻辑见交互段） */
		if (show_mini)
		{
			draw_list->AddRectFilled(ImVec2(mini_x0, mini_y0), ImVec2(mini_x1, mini_y1),
				ImGui::GetColorU32(EditorTheme::Token::Neutral0), 3.0f);
			draw_list->AddRect(ImVec2(mini_x0, mini_y0), ImVec2(mini_x1, mini_y1),
				ImGui::GetColorU32(EditorTheme::Token::Border), 3.0f);

			uint32_t mini_max_depth = 0;
			for (const FlameBlock& block : blocks)
				mini_max_depth = ImMax(mini_max_depth, block.Depth);
			const float mini_row_h = ImMax(
				(mini_y1 - mini_y0) / static_cast<float>(mini_max_depth + 1), 1.0f);
			for (const FlameBlock& block : blocks)
			{
				const float block_x0 = mini_x0 + static_cast<float>(block.X0) * mini_w;
				const float block_x1 = mini_x0 + static_cast<float>(block.X1) * mini_w;
				const float block_y = mini_y0 + static_cast<float>(block.Depth) * mini_row_h;
				draw_list->AddRectFilled(ImVec2(block_x0, block_y),
					ImVec2(ImMax(block_x1, block_x0 + 1.0f), ImMin(block_y + mini_row_h, mini_y1)),
					HeatFillForUs(block.Node->TotalUs));
			}

			const ImVec2 thumb = mini_thumb();
			ImVec4 highlight = EditorTheme::Token::AccentHover;
			highlight.w *= 0.28f;
			draw_list->AddRectFilled(ImVec2(thumb.x, mini_y0), ImVec2(thumb.x + thumb.y, mini_y1),
				ImGui::GetColorU32(highlight), 3.0f);
			draw_list->AddRect(ImVec2(thumb.x, mini_y0), ImVec2(thumb.x + thumb.y, mini_y1),
				ImGui::GetColorU32(EditorTheme::Token::AccentHover), 3.0f, 0, 1.5f);
		}

		if (hover_block != nullptr)
		{
			const ResultCPUTimerNode& node = *hover_block->Node;
			draw_list->AddRect(hover_min, hover_max, ImGui::GetColorU32(EditorTheme::Token::AccentHover),
				1.5f, 0, 2.0f);
			ImGui::BeginTooltip();
			ImGui::Text("%s", node.Label.c_str());
			TimingLocationText(node);
			ImGui::Text("total %.2f us   self %.2f us", node.TotalUs, node.SelfUs);
			ImGui::Text("span %.1f - %.1f us · %u calls", node.BeginUs, node.EndUs, node.CallCount);
			if (!node.Children.empty() && hover_block->ParentBlock >= 0)
				ImGui::TextDisabled(hover_block->Collapsed ? "Click to expand" : "Click to collapse");
			ImGui::TextDisabled("Wheel: zoom · Drag: pan · Right-click: reset");
			ImGui::EndTooltip();
		}

		/* 单击条目（位移未过阈值就松开）：收起 / 展开子树（键 = 「显示根 → 条目」的绝对
		 * label 链；显示根不响应）。滑动条上的按下不算单击（ScrollGrab）。 */
		if (ImGui::IsItemDeactivated() && m_CpuFlameDraggedDistance <= 3.0f && !m_CpuFlameScrollGrab
			&& hover_block != nullptr && !hover_block->Node->Children.empty()
			&& hover_block->ParentBlock >= 0)
		{
			std::string path;
			for (int32_t cursor = static_cast<int32_t>(hover_block - blocks.data()); cursor >= 0;
				cursor = blocks[cursor].ParentBlock)
			{
				if (!path.empty())
					path.insert(0, "/");
				path.insert(0, blocks[cursor].Node->Label);
			}
			if (m_CpuFlameCollapsed.count(path) > 0)
				m_CpuFlameCollapsed.erase(path);
			else
				m_CpuFlameCollapsed.insert(path);
		}

		/* 滑动条抓取标记在收起判定之后清掉（判定用其快照） */
		if (ImGui::IsItemDeactivated())
			m_CpuFlameScrollGrab = false;
	}

	/* 图表的显示根（卡与放大窗口共用）：录制视图 = 选中录制帧（无效时机返回空）；
	 * 其余 = 实时树（EMA） */
	const ResultCPUTimerNode* RenderStatsPanel::ResolveDisplayRoot() const
	{
		/* 抓取冻结中（仅实时视图）：显示根改用当前帧快照，不再跟随实时数据刷新 */
		if (!m_CpuRecordView && m_CpuFrozen)
			return &m_CpuFrozenRoot;

		const CPUTimerRecorder& recorder = CPUTimerProfiler::Instance().GetRecorder();
		if (m_CpuRecordView)
		{
			if (!recorder.IsRecording() && m_CpuReplayFrame >= 0
				&& m_CpuReplayFrame < static_cast<int>(recorder.GetFrameCount()))
				return &recorder.GetFrame(static_cast<uint32_t>(m_CpuReplayFrame));
			return nullptr;
		}
		return &CPUTimerProfiler::Instance().GetRealtimeRoot();
	}

	/* 图表放大窗口：「在窗口中打开」弹出的独立顶层窗口（可移动 / 缩放 / 最大化，标题栏 X
	 * 可关）。数据源与卡一致；图表视图与卡片独立（两处切换条各滑各的，下钻 / 折叠 / 缩放
	 * 等状态仍共享）。打开期间卡片里的图表让位（同一份数据只显示一处；收窗即恢复）。 */
	void RenderStatsPanel::ShowCPUChartWindow()
	{
		PROFILE_FUNCTION();

		if (!m_CpuChartWindowOpen)
			return;

		ImGui::SetNextWindowPos(ImVec2(340.0f, 40.0f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(920.0f, 600.0f), ImGuiCond_FirstUseEver);
		if (m_CpuChartWindowMaximized)
		{
			/* 最大化：每帧强压到主视口工作区（跟随编辑器窗口尺寸变化；WorkPos 已扣掉
			 * 主菜单栏，不会盖住菜单）。被强制期间用户拖 / 拉都会在下帧弹回。 */
			const ImGuiViewport* viewport = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
			ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
		}
		else if (m_CpuChartWindowRestorePending)
		{
			/* 还原：把最大化前记下的浮动矩形写回（只此一帧，之后交回用户拖拽 / 缩放） */
			ImGui::SetNextWindowPos(ImVec2(m_CpuChartRestoreX, m_CpuChartRestoreY), ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(m_CpuChartRestoreW, m_CpuChartRestoreH), ImGuiCond_Always);
			m_CpuChartWindowRestorePending = false;
		}
		if (ImGui::Begin("CPU Chart", &m_CpuChartWindowOpen))
		{
			const ResultCPUTimerNode* display_root = ResolveDisplayRoot();
			if (display_root == nullptr || display_root->Label.empty())
			{
				ImGuiExt::DrawCommonTextUI("Timers", "No data");
			}
			else
			{
				/* 视图切换（紧凑宽度）+ 行内搜索（右对齐）+ 行尾 [抓取当前帧][最大化 / 还原]
				 *（与卡同一版式） */
				const ImGuiStyle& row_style = ImGui::GetStyle();
				const float row_width = ImGui::GetContentRegionAvail().x;
				const float max_button_size = ImGui::GetFrameHeight();
				/* 抓取钮位：录制视图不出现（与卡一致） */
				const float capture_size = max_button_size;
				const float capture_reserve = !m_CpuRecordView
					? capture_size + row_style.ItemSpacing.x : 0.0f;
				const float switcher_width = ImMin(TimingSwitcherWidth(),
					ImMax(row_width - 70.0f - max_button_size - capture_reserve
						- row_style.ItemSpacing.x * 3.0f, 0.0f));
				const float search_width = TimingSearchWidth(row_width, switcher_width,
					max_button_size + capture_reserve);
				const int switched_view = SlidingTabTriple("##CPUChartTabsWindow",
					"Tree", Icons::Id::Menu, "Treemap", Icons::Id::Grid, "Flame", Icons::Id::Stats,
					static_cast<int>(m_CpuChartViewWindow), m_CpuChartSlideWindow, switcher_width);
				if (switched_view >= 0)
					m_CpuChartViewWindow = static_cast<CpuChartView>(switched_view);

				/* 帧号（切换条后面）：录制回放 = 该帧的引擎帧号；抓取冻结中 = 抓取时刻的帧号
				 *（不随实时跳动）；实时 = 当前帧号 */
				{
					CPUTimerProfiler& profiler = CPUTimerProfiler::Instance();
					const CPUTimerRecorder& recorder = profiler.GetRecorder();
					uint32_t frame_id = profiler.GetFrameId();
					if (!m_CpuRecordView && m_CpuFrozen)
						frame_id = m_CpuFrozenFrameId;
					else if (m_CpuRecordView && m_CpuReplayFrame >= 0
						&& m_CpuReplayFrame < static_cast<int>(recorder.GetFrameCount()))
						frame_id = recorder.GetFrameId(static_cast<uint32_t>(m_CpuReplayFrame));
					SameLineAt(switcher_width + row_style.ItemSpacing.x * 2.0f);
					/* 与切换条的竖直中心对齐：SameLine 默认贴行顶、文字比图标矮会整体偏上，
					 * 按切换条实际高度补半格差（GetItemRectSize 此刻 = 切换条） */
					ImGui::SetCursorPosY(ImGui::GetCursorPosY()
						+ (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f);
					ImGui::TextDisabled("Frame %u", frame_id);
				}

				SameLineAt(row_width - search_width - max_button_size - capture_reserve - row_style.ItemSpacing.x);
				TimingSearchInput("##CPUTimingSearchWindow", m_CpuTimingSearch,
					IM_ARRAYSIZE(m_CpuTimingSearch), m_CpuTimingSearchFilter,
					m_CpuTimingSearchMatchCount, search_width);

				/* 抓取当前帧（搜索右侧、实时视图；与卡的钮共享同一冻结态） */
				if (!m_CpuRecordView)
				{
					SameLineAt(row_width - max_button_size - capture_size - row_style.ItemSpacing.x);
					if (Icons::IconButton(Icons::Id::Capture, ImVec2(capture_size, capture_size), m_CpuFrozen,
						m_CpuFrozen ? "Resume live updates" : "Capture current frame (freeze for inspection)"))
						ToggleCPUCaptureFrame();
				}

				SameLineAt(row_width - max_button_size);
				if (Icons::IconButton(m_CpuChartWindowMaximized ? Icons::Id::Restore : Icons::Id::Maximize,
					ImVec2(max_button_size, max_button_size), m_CpuChartWindowMaximized,
					m_CpuChartWindowMaximized ? "Restore to floating window" : "Maximize (fill the editor window)"))
					ToggleCPUChartWindowMaximized();

				const float chart_width = ImGui::GetContentRegionAvail().x;
				const float chart_height = ImGui::GetContentRegionAvail().y;
				switch (m_CpuChartViewWindow)
				{
				case CpuChartView::Treemap:
					ShowCPUTreemapView(*display_root, chart_width, chart_height, true);
					break;
				case CpuChartView::Flame:
					ShowCPUFlameView(*display_root, chart_width, chart_height);
					break;
				case CpuChartView::Tree:
				default:
					ShowCPUTreeView(*display_root);
					break;
				}
			}
		}
		ImGui::End();
	}

	/* 弹窗最大化 / 还原切换（行尾按钮触发）：最大化前先记下当前浮动矩形 —— 供还原写回
	 *（最大化期间窗口自身存的尺寸每帧被覆盖，不能指望 ImGui 记住浮动时的值）。 */
	void RenderStatsPanel::ToggleCPUChartWindowMaximized()
	{
		if (!m_CpuChartWindowMaximized)
		{
			const ImVec2 pos = ImGui::GetWindowPos();
			const ImVec2 size = ImGui::GetWindowSize();
			m_CpuChartRestoreX = pos.x;
			m_CpuChartRestoreY = pos.y;
			m_CpuChartRestoreW = size.x;
			m_CpuChartRestoreH = size.y;
			m_CpuChartWindowMaximized = true;
		}
		else
		{
			m_CpuChartWindowMaximized = false;
			m_CpuChartWindowRestorePending = true;
		}
	}

	/* 抓取当前帧切换（CPU 卡行 / 弹窗行共用）：冻结拍下实时树快照 + 记下抓取时刻的帧号
	 *（暂停期间帧号浮标停在它上面，不再随实时跳动）；再点解冻回实时。 */
	void RenderStatsPanel::ToggleCPUCaptureFrame()
	{
		m_CpuFrozen = !m_CpuFrozen;
		if (m_CpuFrozen)
		{
			CPUTimerProfiler& profiler = CPUTimerProfiler::Instance();
			m_CpuFrozenRoot = profiler.GetRealtimeRoot();
			m_CpuFrozenFrameId = profiler.GetFrameId();
		}
	}

	/* 计时树搜索过滤（自检 / 外部驱动）：写输入缓冲并即时生效（空串 = 清空过滤） */
	void RenderStatsPanel::SetCPUTimingSearchFilter(const std::string& filter)
	{
		m_CpuTimingSearchFilter = ToLowercase(filter.c_str());
		std::snprintf(m_CpuTimingSearch, sizeof(m_CpuTimingSearch), "%s", filter.c_str());
	}

	void RenderStatsPanel::SetGPUTimingSearchFilter(const std::string& filter)
	{
		m_GpuTimingSearchFilter = ToLowercase(filter.c_str());
		std::snprintf(m_GpuTimingSearch, sizeof(m_GpuTimingSearch), "%s", filter.c_str());
	}

	/* 追踪文本会话与录制同起同停（状态驱动、每帧一次）：录制中且勾选 → 在写；否则收尾
	 * 本面板开的会话（谁开谁关，不动外部手动开的）。与录制同生命周期可保证 json 时间线
	 * 覆盖的正是录下来的那些帧（含录满自停）。 */
	void RenderStatsPanel::SyncCPUTraceTextSession()
	{
	#if HELIOS_PROFILE
		const bool recording = CPUTimerProfiler::Instance().GetRecorder().IsRecording();
		TimeCostProfiler& trace = TimeCostProfiler::Instance();
		if (recording && m_CpuTraceTextEnabled && !trace.IsSessionActive())
		{
			trace.BeginSession("Runtime", "TimeCostProfiler-Runtime.json");
			m_CpuTraceTextSessionOwned = true;
		}
		else if ((!recording || !m_CpuTraceTextEnabled) && m_CpuTraceTextSessionOwned)
		{
			trace.EndSession();
			m_CpuTraceTextSessionOwned = false;
		}
	#endif
	}

	/* CPU 录制工具栏：追踪文本开关 + 录制 / 停止、逐帧导航、峰值 / 谷值、清空 —— 一行等大
	 * 图标按钮（宽度均分内容区；无内容 / 录制中时回放按钮置灰）。导出留待下一轮。 */
	void RenderStatsPanel::ShowCPUTimerToolbar()
	{
		PROFILE_FUNCTION();

		CPUTimerRecorder& recorder = CPUTimerProfiler::Instance().GetRecorder();
		const uint32_t count = recorder.GetFrameCount();
		const bool recording = recorder.IsRecording();
		const int last_index = static_cast<int>(count) - 1;

		const float button_gap = ImGui::GetStyle().ItemSpacing.x;
		const float button_width = (ImGui::GetContentRegionAvail().x - button_gap * 6.0f) / 7.0f;
		const ImVec2 button_size(button_width, ImGui::GetFrameHeight() * 1.5f);

	#if HELIOS_PROFILE
		/* 追踪文本开关（录制选项：与录制同起同停写盘，见 SyncCPUTraceTextSession；
		 * 未录制时只记偏好）——工具栏首钮 */
		if (Icons::IconButton(Icons::Id::TraceText, button_size, m_CpuTraceTextEnabled,
			m_CpuTraceTextEnabled
				? "Trace text: ON (recording writes TimeCostProfiler-Runtime.json)"
				: "Trace text: OFF (also write per-function timings while recording)"))
			m_CpuTraceTextEnabled = !m_CpuTraceTextEnabled;
	#endif

		/* 录制 / 停止（同一按钮位，按状态换图标与提示） */
		ImGui::SameLine(0.0f, button_gap);
		if (recording)
		{
			if (Icons::IconButton(Icons::Id::Stop, button_size, false, "Stop recording (keep captured frames)"))
				recorder.Stop();
		}
		else if (Icons::IconButton(Icons::Id::Record, button_size, false, "Start recording (per-frame raw values)"))
		{
			recorder.Start();
			m_CpuReplayFrame = -1; /* 新录制从空内容开始 */
		}

		/* 回放按钮：无内容 / 录制中不可用 */
		ImGui::BeginDisabled(recording || count == 0);
		ImGui::SameLine(0.0f, button_gap);
		if (Icons::IconButton(Icons::Id::PreviousFrame, button_size, false, "Previous frame"))
			m_CpuReplayFrame = m_CpuReplayFrame >= 0 ? ImMax(m_CpuReplayFrame - 1, 0) : last_index;
		ImGui::SameLine(0.0f, button_gap);
		if (Icons::IconButton(Icons::Id::NextFrame, button_size, false, "Next frame"))
			m_CpuReplayFrame = m_CpuReplayFrame >= 0 ? ImMin(m_CpuReplayFrame + 1, last_index) : last_index;
		ImGui::SameLine(0.0f, button_gap);
		if (Icons::IconButton(Icons::Id::Peak, button_size, false, "Peak frame"))
		{
			int best = 0;
			for (int index = 1; index <= last_index; ++index)
				if (recorder.GetFrame(static_cast<uint32_t>(index)).TotalUs
					> recorder.GetFrame(static_cast<uint32_t>(best)).TotalUs)
					best = index;
			m_CpuReplayFrame = best;
		}
		ImGui::SameLine(0.0f, button_gap);
		if (Icons::IconButton(Icons::Id::Valley, button_size, false, "Valley frame"))
		{
			int best = 0;
			for (int index = 1; index <= last_index; ++index)
				if (recorder.GetFrame(static_cast<uint32_t>(index)).TotalUs
					< recorder.GetFrame(static_cast<uint32_t>(best)).TotalUs)
					best = index;
			m_CpuReplayFrame = best;
		}
		ImGui::SameLine(0.0f, button_gap);
		if (Icons::IconButton(Icons::Id::Remove, button_size, false, "Clear recording"))
		{
			recorder.Clear();
			m_CpuReplayFrame = -1;
		}
		ImGui::EndDisabled();
	}

	/* CPU 录制的逐帧回放：帧间总耗时曲线（点击/拖动选帧 + 上限刻度）→ 选帧滑条 →
	 * 概要行；录制树里是逐帧原始值（无平滑）。与 GPU 版同一套交互，仅数据源不同。 */
	void RenderStatsPanel::ShowCPUTimerRecordingUI()
	{
		PROFILE_FUNCTION();

		const CPUTimerRecorder& recorder = CPUTimerProfiler::Instance().GetRecorder();
		const uint32_t count = recorder.GetFrameCount();
		if (count == 0)
			return;
		const int last_index = static_cast<int>(count) - 1;
		const ImGuiStyle& style = ImGui::GetStyle();

		/* ---- 帧间总耗时曲线：一眼看出尖峰分布；点击/拖动选帧 ---- */
		const float width = ImGui::GetContentRegionAvail().x;
		constexpr float kGraphHeight = 60.0f;
		const ImVec2 graph_min = ImGui::GetCursorScreenPos();
		const ImVec2 graph_max(graph_min.x + width, graph_min.y + kGraphHeight);

		ImGui::InvisibleButton("##CPURecordingGraph", ImVec2(width, kGraphHeight));
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
			value_max = ImMax(value_max, static_cast<float>(recorder.GetFrame(index).TotalUs));
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
			[&](uint32_t index) { return ImVec2(column_x(index), value_y(recorder.GetFrame(index).TotalUs)); },
			ImGui::GetColorU32(kCpuLineColor), 1.2f);

		/* 上限刻度：左上角小字 */
		char scale_text[32];
		snprintf(scale_text, sizeof(scale_text), "%.0f us", value_max);
		draw_list->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.85f,
			ImVec2(plot_min.x + 3.0f, plot_min.y),
			ImGui::GetColorU32(EditorTheme::Token::TextDim), scale_text);

		/* 选中帧游标 */
		if (m_CpuReplayFrame >= 0)
		{
			const float cursor_x = column_x(static_cast<uint32_t>(m_CpuReplayFrame));
			draw_list->AddLine(ImVec2(cursor_x, plot_min.y), ImVec2(cursor_x, plot_max.y),
				ImGui::GetColorU32(EditorTheme::Token::Accent), 1.0f);
		}

		/* 点击 / 拖动 = 选帧（按列就近取整） */
		if (ImGui::IsItemActive())
			m_CpuReplayFrame = index_at_mouse();
		else if (graph_hovered)
		{
			const int index = index_at_mouse();
			ImGui::BeginTooltip();
			ImGui::Text("Frame #%u", recorder.GetFrameId(static_cast<uint32_t>(index)));
			ImGui::Text("%.0f us", recorder.GetFrame(static_cast<uint32_t>(index)).TotalUs);
			ImGui::EndTooltip();
		}

		/* ---- 选帧滑条：图表下方 ---- */
		if (last_index > 0)
		{
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
				ImVec2(style.ItemSpacing.x, style.ItemSpacing.y * 0.5f));
			int frame_value = m_CpuReplayFrame >= 0 ? m_CpuReplayFrame : last_index;
			if (FrameSlider("##CPURecordingFrame", frame_value, 0, last_index))
				m_CpuReplayFrame = frame_value;
			ImGui::PopStyleVar();
		}

		/* ---- 概要：帧数 + 峰值帧 ---- */
		int peak_index = 0;
		for (uint32_t index = 1; index < count; ++index)
			if (recorder.GetFrame(index).TotalUs
				> recorder.GetFrame(static_cast<uint32_t>(peak_index)).TotalUs)
				peak_index = static_cast<int>(index);
		ImGui::TextDisabled("%u frames   peak %.0f us @ #%u", count,
			recorder.GetFrame(static_cast<uint32_t>(peak_index)).TotalUs,
			recorder.GetFrameId(static_cast<uint32_t>(peak_index)));
	}

	/* GPU 计时卡：开关 + 逐层耗时表（结果树；无数据节点以占位显示） */
	void RenderStatsPanel::ShowGPUTimingsCard()
	{
		PROFILE_FUNCTION();

		const PanelChrome::Card card = PanelChrome::BeginCard("GPU Timings", Icons::Id::Stats);

		if (card.Open)
		{
			/* 与 CPU 卡同理：卡内控件套一层 ID 作用域，别和上面那张卡的按钮撞 id */
			ImGui::PushID("GPU Timings");

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
				const float kBarHeight = Scaled(3.0f);
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
				display_root = m_GpuFrozen ? &m_GpuFrozenRoot : &Renderer::GetGPUTimerRoot();

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
				/* 搜索行（与 CPU 卡同款）：命中计数每帧随树结算；实时视图在右端留抓取钮位 */
				m_GpuTimingSearchMatchCount = TimingMatchCount(*display_root, m_GpuTimingSearchFilter);
				const bool gpu_filtering = !m_GpuTimingSearchFilter.empty();
				const ImGuiStyle& gpu_style = ImGui::GetStyle();
				const float gpu_row_width = ImGui::GetContentRegionAvail().x;
				const float gpu_capture_size = ImGui::GetFrameHeight();
				const bool gpu_can_capture = !m_RecordView;
				const float gpu_capture_reserve = gpu_can_capture
					? gpu_capture_size + gpu_style.ItemSpacing.x : 0.0f;
				TimingSearchRow("##GPUTimingSearch", m_GpuTimingSearch,
					IM_ARRAYSIZE(m_GpuTimingSearch), m_GpuTimingSearchFilter, m_GpuTimingSearchMatchCount,
					gpu_capture_reserve);

				/* 抓取当前帧（搜索右侧、实时视图）：冻结 = 显示根改用快照供逐行分析；再点解冻 */
				if (gpu_can_capture)
				{
					SameLineAt(gpu_row_width - gpu_capture_size);
					if (Icons::IconButton(Icons::Id::Capture,
						ImVec2(gpu_capture_size, gpu_capture_size), m_GpuFrozen,
						m_GpuFrozen ? "Resume live updates" : "Capture current frame (freeze for inspection)"))
					{
						m_GpuFrozen = !m_GpuFrozen;
						if (m_GpuFrozen)
							m_GpuFrozenRoot = Renderer::GetGPUTimerRoot();
					}
				}

				std::function<void(const ResultGPUTimerNode&)> show_node_recursively;
				show_node_recursively = [&show_node_recursively, gpu_filtering, this](const ResultGPUTimerNode& timer_node)
					{
						if (timer_node.Label.empty())
							return;

						/* 搜索过滤：自身与后代都无命中的整支不画（过滤清空时恒可见） */
						if (gpu_filtering && !TimingSubtreeMatches(timer_node, m_GpuTimingSearchFilter))
							return;

						char label[64] = {};
						snprintf(label, sizeof(label), "(%d) %s", timer_node.QueryIndex, timer_node.Label.c_str());
						label[sizeof(label) - 1] = 0;

						const bool has_children = !timer_node.Children.empty();

						/* 浅层默认展开；点行任意一处（含箭头那一下）都选中这行
						 *（无效 QueryIndex 转 int 恰是 -1 = 未选中哨兵，天然对得上）；
						 * 过滤中：命中路径上的祖父行强制展开 */
						const TimingRowState row = TimingTreeRow(
							reinterpret_cast<void*>(static_cast<intptr_t>(timer_node.QueryIndex)),
							label, has_children, true,
							m_SelectedNode == static_cast<int>(timer_node.QueryIndex),
							gpu_filtering && !TimingNodeMatches(timer_node, m_GpuTimingSearchFilter));
						if (row.Clicked)
							m_SelectedNode = static_cast<int>(timer_node.QueryIndex);

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
					show_node_recursively(*display_root);
					ImGui::EndTable();
				}
			}

			ImGui::PopID();
		}

		PanelChrome::EndCard(card);
	}

	/* 录制工具栏：录制 / 停止、上一帧 / 下一帧、峰值 / 谷值、清空、导出 —— 一行等大图标按钮
	 * （宽度均分内容区；无内容 / 录制中时回放与导出按钮置灰）。 */
	void RenderStatsPanel::ShowGPUTimerToolbar()
	{
		PROFILE_FUNCTION();

		GPUTimerRecorder& recorder = RenderQueryProfiler::Instance().GetRecorder();
		const uint32_t count = recorder.GetFrameCount();
		const bool recording = recorder.IsRecording();
		const int last_index = static_cast<int>(count) - 1;

		const float button_gap = ImGui::GetStyle().ItemSpacing.x;
		const float button_width = (ImGui::GetContentRegionAvail().x - button_gap * 6.0f) / 7.0f;
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
		ImGui::SameLine(0.0f, button_gap);
		/* 导出：录制内容写 CSV（宽表见 BuildRecordingCsv）；落盘路径交给系统保存对话框。
		 * 保存框不保证带后缀（macOS 面板不会自动加）—— 落盘前统一收尾补 ".csv"。 */
		if (Icons::IconButton(Icons::Id::Export, button_size, false, "Export recording (CSV)"))
		{
			const std::string path = EnsureRecordingExtension(FileDialog::SaveFile("csv(*.csv)\0*.csv\0"));
			const std::string csv = path.empty() ? std::string() : BuildRecordingCsv(recorder);
			if (!csv.empty())
			{
				std::ofstream out(PathFromUtf8(path), std::ios::binary);
				out.write(csv.data(), static_cast<std::streamsize>(csv.size()));
				if (out.good())
					EDITOR_LOG_INFO("GPU recording exported: \"{}\" ({} frames).", path, count);
				else
					EDITOR_LOG_ERROR("Failed to export GPU recording to \"{}\".", path);
			}
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
		const float kGraphHeight = Scaled(60.0f);
		const ImVec2 graph_min = ImGui::GetCursorScreenPos();
		const ImVec2 graph_max(graph_min.x + width, graph_min.y + kGraphHeight);

		ImGui::InvisibleButton("##GPURecordingGraph", ImVec2(width, kGraphHeight));
		const bool graph_hovered = ImGui::IsItemHovered();

		ImDrawList* draw_list = ImGui::GetWindowDrawList();
		draw_list->AddRectFilled(graph_min, graph_max, ImGui::GetColorU32(EditorTheme::Token::Neutral1), Scaled(3.0f));
		draw_list->AddRect(graph_min, graph_max, ImGui::GetColorU32(EditorTheme::Token::Border), Scaled(3.0f));

		const float kPadX = Scaled(4.0f);
		const float kPadY = Scaled(3.0f);
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
			ImGui::GetColorU32(kGpuLineColor), Scaled(1.2f));

		/* 上限刻度：左上角小字（与 Frame 卡曲线同一笔账） */
		char scale_text[32];
		snprintf(scale_text, sizeof(scale_text), "%.0f us", value_max);
		draw_list->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.85f,
			ImVec2(plot_min.x + Scaled(3.0f), plot_min.y),
			ImGui::GetColorU32(EditorTheme::Token::TextDim), scale_text);

		/* 选中帧游标 */
		if (m_ReplayFrame >= 0)
		{
			const float cursor_x = column_x(static_cast<uint32_t>(m_ReplayFrame));
			draw_list->AddLine(ImVec2(cursor_x, plot_min.y), ImVec2(cursor_x, plot_max.y),
				ImGui::GetColorU32(EditorTheme::Token::Accent), Scaled(1.0f));
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

	/* GPU 录制导出（CSV 宽表：一行一帧）：帧号 · 根总耗时（v / f 细分）· 每个作用域节点两列
	 * （总耗时 + 顶点细分，片元 = 总 − 顶点）。列名 = 标签路径 + ".total_us" / ".vertex_us"；
	 * 节点集合取全部录制帧的并集（缺失留空）、列序 = 首次出现的先序。纯函数：headless 能直接断言。 */
	std::string RenderStatsPanel::BuildRecordingCsv(const GPUTimerRecorder& recorder)
	{
		const uint32_t count = recorder.GetFrameCount();
		if (count == 0)
			return {};

		/* 一、收集：节点列顺序（首现先序）+ 每帧的 (节点下标 → 总 / 顶点耗时之和) */
		struct NodeValues
		{
			double Total = 0.0;
			double Vertex = 0.0;
		};

		std::vector<std::string> nodes;
		std::unordered_map<std::string, size_t> node_index;
		std::vector<std::unordered_map<size_t, NodeValues>> frame_values(count);

		std::function<void(const ResultGPUTimerNode&, const std::string&, std::unordered_map<size_t, NodeValues>&)> collect;
		collect = [&node_index, &nodes, &collect](const ResultGPUTimerNode& node, const std::string& parent_path,
			std::unordered_map<size_t, NodeValues>& values)
			{
				for (const ResultGPUTimerNode& child : node.Children)
				{
					/* 空标签节点不做列，用父路径继续向子层递归 */
					if (child.Label.empty())
					{
						collect(child, parent_path, values);
						continue;
					}

					const std::string path = parent_path.empty() ? child.Label : parent_path + "/" + child.Label;
					const auto [entry, inserted] = node_index.try_emplace(path, nodes.size());
					if (inserted)
						nodes.emplace_back(path);
					if (child.HasData)
					{
						NodeValues& value = values[entry->second]; /* 同路径重复出现 = 求和 */
						value.Total += child.GPUTime;
						value.Vertex += child.GPUTimeVertex;
					}

					collect(child, path, values);
				}
			};

		for (uint32_t index = 0; index < count; ++index)
			collect(recorder.GetFrame(index), {}, frame_values[index]);

		/* 二、写表：UTF-8 BOM（Excel 打开中文实体名不乱码）→ 表头 → 每帧一行 */
		std::string csv;
		csv.reserve(256 + static_cast<size_t>(count) * (32 + nodes.size() * 24));

		const auto append_field = [&csv](const std::string& field)
			{
				/* 含分隔符 / 引号 / 换行 / 首尾空格 → RFC 4180 引号包裹（引号翻倍） */
				const bool needs_quotes = field.find_first_of(",\"\n\r") != std::string::npos
					|| (!field.empty() && (field.front() == ' ' || field.back() == ' '));
				if (!needs_quotes)
				{
					csv += field;
					return;
				}

				csv += '"';
				for (const char ch : field)
				{
					if (ch == '"')
						csv += '"';
					csv += ch;
				}
				csv += '"';
			};

		csv += "\xEF\xBB\xBF";
		append_field("frame");
		csv += ',';
		append_field("total_us");
		csv += ',';
		append_field("vertex_us");
		csv += ',';
		append_field("fragment_us");
		for (const std::string& node : nodes)
		{
			csv += ',';
			append_field(node + ".total_us");
			csv += ',';
			append_field(node + ".vertex_us");
		}
		csv += '\n';

		char cell[32];
		for (uint32_t index = 0; index < count; ++index)
		{
			const ResultGPUTimerNode& frame = recorder.GetFrame(index);

			snprintf(cell, sizeof(cell), "%u", recorder.GetFrameId(index));
			csv += cell;

			if (frame.HasData)
			{
				snprintf(cell, sizeof(cell), ",%.3f,%.3f,%.3f",
					frame.GPUTime, frame.GPUTimeVertex, frame.GPUTimeFragment);
				csv += cell;
			}
			else
			{
				csv += ",,,"; /* 无数据帧：占位留空（写 0 会被当成真实零耗时） */
			}

			for (size_t node = 0; node < nodes.size(); ++node)
			{
				csv += ',';
				const auto value = frame_values[index].find(node);
				if (value != frame_values[index].end())
				{
					snprintf(cell, sizeof(cell), "%.3f", value->second.Total);
					csv += cell;
				}

				csv += ',';
				if (value != frame_values[index].end())
				{
					snprintf(cell, sizeof(cell), "%.3f", value->second.Vertex);
					csv += cell;
				}
			}
			csv += '\n';
		}

		return csv;
	}

	/* 导出落盘的路径收尾：系统保存框不保证补后缀 —— 名称没以 ".csv"（大小写不敏感）结尾
	 * 就补上，已有则不重复；空路径（用户取消）原样返回。纯函数：headless 直接断言。 */
	std::string RenderStatsPanel::EnsureRecordingExtension(const std::string& path)
	{
		constexpr const char* kExtension = ".csv";
		constexpr size_t kExtensionLength = 4;

		if (path.size() >= kExtensionLength)
		{
			const auto ascii_lower = [](char ch)
				{ return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; };

			bool has_extension = true;
			for (size_t index = 0; index < kExtensionLength; ++index)
			{
				if (ascii_lower(path[path.size() - kExtensionLength + index]) != kExtension[index])
				{
					has_extension = false;
					break;
				}
			}
			if (has_extension)
				return path;
		}

		return path.empty() ? path : path + kExtension;
	}
}
