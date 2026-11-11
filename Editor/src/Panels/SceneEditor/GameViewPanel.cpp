#include "Pch.h"
#include "GameViewPanel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "EditorUI/EditorTheme.h"
#include "EditorUI/ImGuiExtensions.h"
#include "EditorUI/PanelChrome.h"

namespace Helios
{
	namespace
	{
		/* 工具行上下留白：面板内边距为 0（画面要贴边），控件不靠留白就会贴顶 */
		float ToolbarPadding() { return Scaled(6.0f); }
		/* 工具行左右内边距：与面板的常规内边距同档 */
		float ToolbarSidePadding() { return Scaled(12.0f); }

		/* 常用画面比例：弹层里按这个顺序排，选中态按数值匹配 */
		struct AspectOption
		{
			const char* Label;
			float Value;
		};

		constexpr AspectOption kAspectOptions[] =
		{
			{ "16:9",  16.0f / 9.0f },
			{ "16:10", 16.0f / 10.0f },
			{ "3:2",   3.0f / 2.0f },
			{ "4:3",   4.0f / 3.0f },
			{ "1:1",   1.0f },
			{ "21:9",  21.0f / 9.0f },
		};

		constexpr float kAspectTolerance = 1e-3f;

		const AspectOption* MatchAspect(float ratio)
		{
			for (const AspectOption& option : kAspectOptions)
			{
				if (std::fabs(option.Value - ratio) <= kAspectTolerance)
					return &option;
			}
			return nullptr;
		}

		/* 比例转整数对：先找最简分数（分母试到 64），找不到按百分之一近似 */
		void AspectToFraction(float ratio, int& width, int& height)
		{
			for (int h = 1; h <= 64; ++h)
			{
				const int w = static_cast<int>(std::lround(ratio * static_cast<float>(h)));
				if (w > 0 && std::fabs(ratio - static_cast<float>(w) / static_cast<float>(h)) <= kAspectTolerance)
				{
					width = w;
					height = h;
					return;
				}
			}

			width = static_cast<int>(std::lround(ratio * 100.0f));
			height = 100;
		}

		/* 按钮标签：Free / 档位名 / 自定义分数 */
		std::string AspectLabel(bool fixed, float ratio)
		{
			if (!fixed || ratio <= 0.0f)
				return "Free";

			if (const AspectOption* option = MatchAspect(ratio))
				return option->Label;

			int width = 16;
			int height = 9;
			AspectToFraction(ratio, width, height);
			return std::to_string(width) + ":" + std::to_string(height);
		}

		/* 比例按钮宽：按最长档位名算，切换档位时不抖动 */
		float AspectButtonWidth()
		{
			float text_width = ImGui::CalcTextSize("16:10").x;
			for (const AspectOption& option : kAspectOptions)
				text_width = ImMax(text_width, ImGui::CalcTextSize(option.Label).x);

			return text_width + ImGui::GetStyle().FramePadding.x * 2.0f + Scaled(12.0f);
		}
	}

	GameViewPanel::Result GameViewPanel::Draw(State& state, ImTextureID image)
	{
		Result result;

		result.Hovered = ImGui::IsWindowHovered();
		/* 弹层（菜单 / 下拉 / 模态）打开时键盘归它，脚本不该继续收键 */
		const bool popup_open = ImGui::IsPopupOpen(nullptr,
			ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
		result.Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !popup_open;

		if (state.Notice != nullptr)
		{
			const ImVec2 min = ImGui::GetCursorScreenPos();
			const ImVec2 size = ImGui::GetContentRegionAvail();
			DrawNotice(state, min, size);
			/* 空态是手动定位画的，光标摆回去再占位，内容高度才等于整块视口 */
			ImGui::SetCursorScreenPos(min);
			ImGui::Dummy(size);
			return result;
		}

		DrawToolbar(state, result);

		const ImVec2 content_min = ImGui::GetCursorScreenPos();
		const ImVec2 content_size = ImGui::GetContentRegionAvail();
		if (content_size.x <= 0.0f || content_size.y <= 0.0f)
			return result;

		/* 视口区域铺最深一档：16:9 的黑边、画面未就绪时的空场都落在它上面 */
		ImDrawList* draw_list = ImGui::GetWindowDrawList();
		const ImVec2 content_max(content_min.x + content_size.x, content_min.y + content_size.y);
		draw_list->AddRectFilled(content_min, content_max, ImGui::GetColorU32(EditorTheme::Token::Neutral0));

		/* 画面区域：默认铺满，固定比例时按比例居中 */
		ImVec2 box_min = content_min;
		ImVec2 box_size = content_size;
		if (state.FixedAspect && state.AspectRatio > 0.0f)
		{
			const float aspect = state.AspectRatio;
			if (box_size.x / box_size.y > aspect)
			{
				box_size.x = box_size.y * aspect;
				box_min.x += (content_size.x - box_size.x) * 0.5f;
			}
			else
			{
				box_size.y = box_size.x / aspect;
				box_min.y += (content_size.y - box_size.y) * 0.5f;
			}
		}

		result.Region.MinX = static_cast<int32_t>(box_min.x);
		result.Region.MinY = static_cast<int32_t>(box_min.y);
		result.Region.Width = static_cast<uint32_t>(std::max(0.0f, box_size.x));
		result.Region.Height = static_cast<uint32_t>(std::max(0.0f, box_size.y));
		result.Drawable = true;

		if (image != nullptr)
		{
			ImGui::SetCursorScreenPos(box_min);
			ImGui::Image(image, box_size, ImVec2{ 0, 1 }, ImVec2{ 1, 0 });
		}

		/* 画面框：运行中描状态色（粗一档），其余时候只是一条细边 */
		const ImVec2 box_max(box_min.x + box_size.x, box_min.y + box_size.y);
		draw_list->AddRect(box_min, box_max,
			ImGui::GetColorU32(state.Playing ? EditorTheme::Token::Playing : EditorTheme::Token::Border),
			0.0f, ImDrawFlags_None, Scaled(state.Playing ? 2.0f : 1.0f));

		ImGui::SetCursorScreenPos(ImVec2(content_min.x, content_max.y));
		return result;
	}

	/* 工具行：左端依次是运行状态、画面比例与时间缩放，右端只留全屏开关 */
	void GameViewPanel::DrawToolbar(State& state, Result& result)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const float button = ImGui::GetFrameHeight();
		const float gap = style.ItemInnerSpacing.x;

		const ImVec2 row_min = ImGui::GetCursorScreenPos();
		const float row_width = ImGui::GetContentRegionAvail().x;
		const float row_height = button + ToolbarPadding() * 2.0f;
		const float left = row_min.x + ToolbarSidePadding();
		const float right = row_min.x + row_width - ToolbarSidePadding();
		const float y = row_min.y + ToolbarPadding();

		ImDrawList* draw_list = ImGui::GetWindowDrawList();
		ImGuiWindow* window = ImGui::GetCurrentWindow();

		/* 右端：全屏开关（全屏里换成还原图标） */
		ImGui::SetCursorScreenPos(ImVec2(right - button, y));
		if (Icons::IconButton(state.Fullscreen ? Icons::Id::Restore : Icons::Id::Maximize,
				ImVec2(button, button), state.Fullscreen,
				state.Fullscreen ? "Exit fullscreen  (Esc)" : "Fullscreen  (Esc to exit)"))
		{
			result.ToggleFullscreen = true;
		}

		/* 全屏按钮让出来的右边界：左端这一排最远到这里 */
		const float line_end = right - button - gap * 2.0f;

		/* ---- 状态点 + 状态文字 ---- */
		const float dot_radius = Scaled(3.5f);
		float x = left;
		ImGui::SetCursorScreenPos(ImVec2(x, y));
		ImGui::Dummy(ImVec2(dot_radius * 2.0f, button));
		draw_list->AddCircleFilled(ImVec2(x + dot_radius, y + button * 0.5f), dot_radius,
			ImGui::GetColorU32(state.Playing ? EditorTheme::Token::Playing : EditorTheme::Token::Neutral7));
		x += dot_radius * 2.0f + gap;

		const char* status_text = state.Paused ? "Paused" : "Playing";
		const float text_y = y + (button - ImGui::GetFontSize()) * 0.5f;
		const float status_width = ImGui::CalcTextSize(status_text).x;

		/* ---- 比例按钮 + 时间缩放读数：这两件始终留着 ---- */
		const float aspect_width = AspectButtonWidth();
		char value_text[16];
		std::snprintf(value_text, sizeof(value_text), "%.2fx", state.TimeScale);
		const float value_width = ImGui::CalcTextSize("0.00x").x;

		/* 退让：状态文字放不下就只留圆点（比例与读数不省） */
		const float rest_width = gap * 2.0f + aspect_width + gap * 2.0f + value_width + gap + Scaled(60.0f);
		if (line_end - x - rest_width >= status_width)
		{
			window->DC.CurrLineTextBaseOffset = 0.0f;
			ImGui::SetCursorScreenPos(ImVec2(x, text_y));
			ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextLabel);
			ImGui::TextUnformatted(status_text);
			ImGui::PopStyleColor();
			x += status_width + gap * 2.0f;
		}

		/* ---- 画面比例紧跟在状态后面 ---- */
		DrawAspectControl(state, ImVec2(x, y), aspect_width, button);
		x += aspect_width + gap * 2.0f;

		/* ---- 时间缩放：细线滑条 + 定宽数值。放不下滑条时只留数值 ---- */
		const float slider_room = line_end - x - value_width - gap;
		if (slider_room >= Scaled(60.0f))
		{
			const float slider_width = ImMin(slider_room, Scaled(120.0f));
			ImGui::SetCursorScreenPos(ImVec2(x, y));
			ImGui::SetNextItemWidth(slider_width);
			ImGuiExt::DrawDotSliderFloat("##TimeScale", state.TimeScale, 0.0f, 4.0f, "%.2fx");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Simulation speed. Rendering stays at full rate.");
			x += slider_width + gap;
		}

		window->DC.CurrLineTextBaseOffset = 0.0f;
		ImGui::SetCursorScreenPos(ImVec2(x, text_y));
		ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
		ImGui::TextUnformatted(value_text);
		ImGui::PopStyleColor();

		/* 收尾：光标落到工具行下方（画面紧贴，靠明度差分开，不再画分隔线） */
		ImGui::SetCursorScreenPos(ImVec2(row_min.x, row_min.y + row_height));
	}

	/* 比例按钮：显示当前档位，点开是预设档位与自定义宽高 */
	void GameViewPanel::DrawAspectControl(State& state, const ImVec2& pos, float width, float height)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		ImGui::SetCursorScreenPos(pos);

		const std::string label = AspectLabel(state.FixedAspect, state.AspectRatio);
		if (ImGui::Button("##Aspect", ImVec2(width, height)))
			ImGui::OpenPopup("##AspectMenu");

		/* 文字左对齐、右端一枚下拉三角：两者各占一边，切换档位时不跳。
		 * 自定义比例的标签可能比按钮宽（1000:999），按按钮矩形裁掉 */
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		ImDrawList* draw_list = ImGui::GetWindowDrawList();
		draw_list->PushClipRect(min, max, true);
		draw_list->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
			ImVec2(min.x + style.FramePadding.x, (min.y + max.y) * 0.5f - ImGui::GetFontSize() * 0.5f),
			ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
		draw_list->PopClipRect();

		const float arrow = Scaled(8.0f);
		const ImVec2 arrow_center(max.x - style.FramePadding.x - arrow * 0.5f, (min.y + max.y) * 0.5f);
		draw_list->AddTriangleFilled(
			ImVec2(arrow_center.x - arrow * 0.5f, arrow_center.y - arrow * 0.3f),
			ImVec2(arrow_center.x + arrow * 0.5f, arrow_center.y - arrow * 0.3f),
			ImVec2(arrow_center.x, arrow_center.y + arrow * 0.4f),
			ImGui::GetColorU32(EditorTheme::Token::TextDim));

		if (ImGui::IsItemHovered())
		{
			if (state.FixedAspect)
				ImGui::SetTooltip("Aspect ratio %s", label.c_str());
			else
				ImGui::SetTooltip("Free fills the panel");
		}

		/* 弹层贴按钮下方、左缘对齐：按钮在左端，向右展开才不会伸出面板 */
		ImGui::SetNextWindowPos(ImVec2(min.x, max.y + Scaled(4.0f)), ImGuiCond_Appearing, ImVec2(0.0f, 0.0f));
		ImGui::SetNextWindowSizeConstraints(ImVec2(Scaled(172.0f), 0.0f), ImVec2(FLT_MAX, FLT_MAX));

		PanelChrome::PushMenuStyle(true);
		if (ImGui::BeginPopup("##AspectMenu"))
		{
			PanelChrome::MenuSectionHeader("ASPECT");
			if (PanelChrome::MenuItemSelectWithIcon(Icons::Id::None, "Free", !state.FixedAspect))
				state.FixedAspect = false;

			for (const AspectOption& option : kAspectOptions)
			{
				const bool selected = state.FixedAspect && MatchAspect(state.AspectRatio) == &option;
				if (PanelChrome::MenuItemSelectWithIcon(Icons::Id::None, option.Label, selected))
				{
					state.FixedAspect = true;
					state.AspectRatio = option.Value;
					/* 自定义那两格跟着走，弹层不收起时看到的就是当前档位的分数 */
					AspectToFraction(state.AspectRatio, m_CustomWidth, m_CustomHeight);
				}
			}

			ImGui::Dummy(ImVec2(0.0f, Scaled(2.0f)));
			ImGui::Separator();
			ImGui::Dummy(ImVec2(0.0f, Scaled(2.0f)));
			PanelChrome::MenuSectionHeader("CUSTOM");

			/* 打开时按当前比例反推宽高，之后跟着输入走 */
			if (ImGui::IsWindowAppearing())
				AspectToFraction(state.AspectRatio, m_CustomWidth, m_CustomHeight);

			const float gap = style.ItemInnerSpacing.x;
			ImGui::Dummy(ImVec2(0.0f, Scaled(2.0f)));
			ImGui::SetNextItemWidth(Scaled(64.0f));
			bool changed = ImGui::DragInt("##AspectWidth", &m_CustomWidth, 1.0f, 1, 1000);
			ImGui::SameLine(0.0f, gap);
			ImGui::TextUnformatted(":");
			ImGui::SameLine(0.0f, gap);
			ImGui::SetNextItemWidth(Scaled(64.0f));
			if (ImGui::DragInt("##AspectHeight", &m_CustomHeight, 1.0f, 1, 1000))
				changed = true;

			if (changed && m_CustomWidth > 0 && m_CustomHeight > 0)
			{
				state.FixedAspect = true;
				state.AspectRatio = static_cast<float>(m_CustomWidth) / static_cast<float>(m_CustomHeight);
			}

			ImGui::EndPopup();
		}
		PanelChrome::PopMenuStyle();
	}

	/* 空态：图标 + 主文案 + 副文案，整组在视口里居中 */
	void GameViewPanel::DrawNotice(const State& state, const ImVec2& min, const ImVec2& size) const
	{
		ImDrawList* draw_list = ImGui::GetWindowDrawList();
		if (size.x <= 0.0f || size.y <= 0.0f)
			return;

		draw_list->AddRectFilled(min, ImVec2(min.x + size.x, min.y + size.y),
			ImGui::GetColorU32(EditorTheme::Token::Neutral0));

		ImGuiWindow* window = ImGui::GetCurrentWindow();
		const float center_x = min.x + size.x * 0.5f;

		const float icon_size = Scaled(40.0f);
		const float gap_icon = Scaled(14.0f);
		const float gap_hint = Scaled(6.0f);

		ImGui::PushFont(EditorTheme::GetFonts().Title);
		const float title_size = ImGui::GetFontSize();
		const ImVec2 title_text = ImGui::CalcTextSize(state.Notice);
		ImGui::PopFont();

		const bool has_hint = state.NoticeHint != nullptr;
		const float hint_size = ImGui::GetFontSize();
		const float total = icon_size + gap_icon + title_size + (has_hint ? gap_hint + hint_size : 0.0f);

		float y = min.y + (size.y - total) * 0.5f;
		Icons::Draw(draw_list, state.NoticeIcon, ImVec2(center_x, y + icon_size * 0.5f), icon_size,
			ImGui::GetColorU32(EditorTheme::WithAlpha(EditorTheme::Token::TextDim, 0.80f)));

		y += icon_size + gap_icon;
		window->DC.CurrLineTextBaseOffset = 0.0f;
		ImGui::SetCursorScreenPos(ImVec2(center_x - title_text.x * 0.5f, y));
		ImGui::PushFont(EditorTheme::GetFonts().Title);
		ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextLabel);
		ImGui::TextUnformatted(state.Notice);
		ImGui::PopStyleColor();
		ImGui::PopFont();

		if (!has_hint)
			return;

		y += title_size + gap_hint;
		const ImVec2 hint_text = ImGui::CalcTextSize(state.NoticeHint);
		window->DC.CurrLineTextBaseOffset = 0.0f;
		ImGui::SetCursorScreenPos(ImVec2(center_x - hint_text.x * 0.5f, y));
		ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
		ImGui::TextUnformatted(state.NoticeHint);
		ImGui::PopStyleColor();
	}
}
