#include "Pch.h"
#include "FrameSlider.h"

namespace Helios
{
	namespace Widgets
	{
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

			const float kGrabPadding = EditorTheme::Scaled(2.0f); /* 与 ImGui 标准滑条的块内边距一致 */
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
}
