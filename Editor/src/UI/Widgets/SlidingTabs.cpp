#include "Pch.h"
#include "SlidingTabs.h"

namespace Helios
{
	namespace Widgets
	{
/* 滑动页签：两个等宽页签共用一条底槽，选中块在两者之间平滑滑动（指数趋近）。
		 * slide 由调用方持久化（0 = 左页签、1 = 右页签），切换时产生滑动动画；
		 * 返回本帧被点击的页签下标（-1 = 未点击）。 */
		int SlidingTabPair(const char* id, const char* label_left, Icons::Id icon_left,
			const char* label_right, Icons::Id icon_right, bool right_selected, float& slide,
			float width_override)
		{
			const ImGuiStyle& style = ImGui::GetStyle();
			const float height = ImGui::GetFrameHeight();
			const float width = width_override > 0.0f ? width_override : ImGui::GetContentRegionAvail().x;
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
				ImGui::GetColorU32(EditorTheme::Token::Border), style.FrameRounding, 0, EditorTheme::Scaled(1.0f));

			/* 未选中格悬停时的提亮底：画在滑动块之下 */
			if (hover_left && right_selected)
				draw_list->AddRectFilled(pos, ImVec2(pos.x + seg_width, pos.y + height),
					ImGui::GetColorU32(EditorTheme::Token::Neutral5), style.FrameRounding);
			if (hover_right && !right_selected)
				draw_list->AddRectFilled(ImVec2(pos.x + seg_width, pos.y), ImVec2(pos.x + width, pos.y + height),
					ImGui::GetColorU32(EditorTheme::Token::Neutral5), style.FrameRounding);

			/* 选中块（滑动中：位置在两格之间插值） */
			const float kBlockInset = EditorTheme::Scaled(2.0f);
			const float block_width = seg_width - kBlockInset * 2.0f;
			const float block_x = pos.x + kBlockInset + seg_width * slide;
			const ImVec2 block_min(block_x, pos.y + kBlockInset);
			const ImVec2 block_max(block_x + block_width, pos.y + height - kBlockInset);
			const float block_rounding = ImMax(EditorTheme::Scaled(1.0f), style.FrameRounding - EditorTheme::Scaled(1.0f));
			draw_list->AddRectFilled(block_min, block_max,
				ImGui::GetColorU32(EditorTheme::WithAlpha(EditorTheme::Token::Accent, 0.17f)), block_rounding);
			draw_list->AddRect(block_min, block_max,
				ImGui::GetColorU32(EditorTheme::WithAlpha(EditorTheme::Token::Accent, 0.65f)),
				block_rounding, 0, EditorTheme::Scaled(1.0f));

			/* 页签内容（图标 + 文字，格内居中）：激活侧高亮；icon = None 时只画文字 */
			const auto draw_tab = [&](float seg_x, const char* label, Icons::Id icon, bool active, bool hovered)
				{
					const float icon_size = height * 0.62f;
					const float kIconGap = EditorTheme::Scaled(6.0f);
					const bool has_icon = icon != Icons::Id::None;
					const ImVec2 label_size = ImGui::CalcTextSize(label);
					const float content_width = has_icon ? icon_size + kIconGap + label_size.x : label_size.x;
					const float content_x = seg_x + (seg_width - content_width) * 0.5f;
					const ImU32 content_color = ImGui::GetColorU32(
						active || hovered ? EditorTheme::Token::Text : EditorTheme::Token::TextDim);
					if (has_icon)
					{
						Icons::Draw(draw_list, icon,
							ImVec2(content_x + icon_size * 0.5f, pos.y + height * 0.5f), icon_size, content_color);
					}
					draw_list->AddText(ImVec2(content_x + (has_icon ? icon_size + kIconGap : 0.0f),
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

		/* 视图切换器（三格）的紧凑宽度：三段各 = 行高 + 8（行太窄时让位 —— 见卡 / 窗行内
		 * 的 ImMin，搜索至少留 70） */
		float TimingSwitcherWidth()
		{
			return (ImGui::GetFrameHeight() + 8.0f) * 3.0f;
		}

		/* 两段开关的紧凑宽度：每格 = 图标 + 间隔 + 最宽标签 + 两侧留白。
		 * 不铺满整行时用它——铺满的两条并列会让人以为是同一级的东西。 */
		float SlidingTabPairWidth(const char* label_left, const char* label_right, bool with_icon)
		{
			const float text_width = ImMax(ImGui::CalcTextSize(label_left).x,
				ImGui::CalcTextSize(label_right).x);
			const float icon_width = with_icon ? ImGui::GetFrameHeight() * 0.62f + EditorTheme::Scaled(6.0f) : 0.0f;
			return (text_width + icon_width + EditorTheme::Scaled(22.0f)) * 2.0f;
		}

		/* 三格滑动页签（树 / 矩形图 / 火焰图）：视觉与 SlidingTabPair 同一套，
		 * 只显示图标、名字走悬浮提示；返回本帧被点击的格（-1 = 无）。 */
		int SlidingTabTriple(const char* id,
			const char* label0, Icons::Id icon0, const char* label1, Icons::Id icon1,
			const char* label2, Icons::Id icon2, int selected, float& slide, float width_override)
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
}
