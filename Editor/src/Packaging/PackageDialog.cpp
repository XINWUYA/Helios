#include "Pch.h"
#include "PackageDialog.h"

#include "Common/EditorCommon.h"
#include "EditorUI/EditorTheme.h"
#include "EditorUI/PanelChrome.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Helios::EditorUI
{
	namespace
	{
		constexpr float kContentWidthDesign = 440.0f;
		/* 标签列只放"位置 / 目录 / 原因"这种短词，比属性面板的 124 窄 */
		constexpr float kLabelWidthDesign = 52.0f;

		/* 属性行：左标签（弱化色）+ 右值，值放不下就省略号收尾，和属性面板同一套节奏。
		 * tooltip 非空时悬停值给全文（长路径在定宽里看不全） */
		void DrawValueRow(const char* label, const std::string& value, const ImVec4& value_color,
			float content_width, const char* tooltip = nullptr)
		{
			const float label_width = Scaled(kLabelWidthDesign);
			const float row_x = ImGui::GetCursorPosX();

			ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextLabel);
			ImGui::TextUnformatted(label);
			ImGui::PopStyleColor();

			ImGui::SameLine(row_x + label_width);
			ImGui::PushStyleColor(ImGuiCol_Text, value_color);
			PanelChrome::DrawClippedTextLine(value.c_str(), content_width - label_width);
			ImGui::PopStyleColor();

			if (tooltip != nullptr && ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", tooltip);
		}

		/* 进度条：轨道与点亮段取滑块那一档颜色，有百分比就按比例填；
		 * 拿不到（CMake 配置阶段不打百分比）就让一块亮条来回滑，至少能看出还在跑 */
		void DrawProgressBar(float fraction, float width)
		{
			const float height = Scaled(4.0f);
			const float rounding = height * 0.5f;
			const ImVec2 min = ImGui::GetCursorScreenPos();
			const ImVec2 max(min.x + width, min.y + height);
			ImDrawList* const draw_list = ImGui::GetWindowDrawList();

			draw_list->AddRectFilled(min, max, ImGui::GetColorU32(EditorTheme::Token::SliderTrack), rounding);

			const ImU32 fill = ImGui::GetColorU32(EditorTheme::Token::SliderValue);
			if (fraction >= 0.0f)
			{
				const float filled = width * std::clamp(fraction, 0.0f, 1.0f);
				if (filled > 0.0f)
					draw_list->AddRectFilled(min, ImVec2(min.x + filled, max.y), fill, rounding);
			}
			else
			{
				const float block = width * 0.24f;
				const float t = std::fmod(static_cast<float>(ImGui::GetTime()) * 0.55f, 1.0f);
				const float x = min.x + (width + block) * t - block;

				draw_list->PushClipRect(min, max, true);
				draw_list->AddRectFilled(ImVec2(x, min.y), ImVec2(x + block, max.y), fill, rounding);
				draw_list->PopClipRect();
			}

			ImGui::Dummy(ImVec2(width, height));
		}

		/* 输出框：比浮层底色深一档的凹陷区，配一档边框，看起来就是个只读文本域 */
		void DrawOutputBox(const std::string& text, float content_width, float height)
		{
			const float pad = Scaled(8.0f);

			ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Token::Neutral2);
			ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextLabel);
			ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Scaled(5.0f));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad * 0.75f));
			ImGui::BeginChild("##package_output", ImVec2(content_width, height), true);
			ImGui::TextUnformatted(text.c_str());
			if (ImGui::IsWindowAppearing())
				ImGui::SetScrollHereY(1.0f); /* 报错通常在末尾 */
			ImGui::EndChild();
			ImGui::PopStyleVar(2);
			ImGui::PopStyleColor(2);
		}
	}

	bool ShowPackageDialog(bool request_open, const PackageDialogModel& model)
	{
		if (request_open && !ImGui::IsPopupOpen("Package Result"))
			ImGui::OpenPopup("Package Result");

		/* 弹层默认落在打开时的光标处，这里压到主视口正中 */
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));

		const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize
			| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;

		if (!ImGui::BeginPopupModal("Package Result", nullptr, flags))
			return false;

		const ImGuiStyle& style = ImGui::GetStyle();
		const float content_width = Scaled(kContentWidthDesign);

		/* 标题行照抄面板头部：标题字号 + 一条分隔线，弹窗因此和面板是同一套骨架 */
		const char* title = "打包完成";
		switch (model.Mode)
		{
		case PackageDialogMode::Progress: title = model.Title.c_str(); break;
		case PackageDialogMode::Failed: title = "打包失败"; break;
		case PackageDialogMode::Environment: title = "无法打包"; break;
		case PackageDialogMode::Success: break;
		}

		const PanelChrome::HeaderRow header = PanelChrome::BeginHeaderRow(Icons::Id::None);
		PanelChrome::DrawHeaderTitle(header, title);
		PanelChrome::EndHeaderRow(header);

		ImGui::Dummy(ImVec2(0.0f, Scaled(3.0f)));

		switch (model.Mode)
		{
		case PackageDialogMode::Progress:
		{
			char status[128];
			if (model.Fraction >= 0.0f)
			{
				std::snprintf(status, sizeof(status), "%s %d%%",
					model.Stage.c_str(), static_cast<int>(model.Fraction * 100.0f + 0.5f));
			}
			else
			{
				std::snprintf(status, sizeof(status), "%s", model.Stage.c_str());
			}

			ImGui::TextUnformatted(status);
			ImGui::Dummy(ImVec2(0.0f, Scaled(6.0f)));
			DrawProgressBar(model.Fraction, content_width);

			ImGui::Dummy(ImVec2(0.0f, Scaled(4.0f)));
			ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
			PanelChrome::DrawClippedTextLine(model.LastLine.c_str(), content_width);
			ImGui::PopStyleColor();
			break;
		}
		case PackageDialogMode::Success:
		{
			const size_t slash = model.Path.find_last_of("/\\");
			const std::string name = (slash == std::string::npos) ? model.Path : model.Path.substr(slash + 1);
			const std::string dir = (slash == std::string::npos) ? std::string() : model.Path.substr(0, slash);

			DrawValueRow("位置", name, EditorTheme::Token::Text, content_width, model.Path.c_str());
			DrawValueRow("目录", dir, EditorTheme::Token::TextDim, content_width, model.Path.c_str());
			break;
		}
		case PackageDialogMode::Failed:
			DrawOutputBox(model.Log, content_width, Scaled(150.0f));
			break;
		case PackageDialogMode::Environment:
			/* 说明用属性值那一档灰：面板里只有"当下就不对"的输入才动红色 */
			ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextLabel);
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_width);
			ImGui::TextUnformatted(model.Issue.c_str());
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
			break;
		}

		ImGui::Dummy(ImVec2(0.0f, Scaled(8.0f)));

		/* 按钮靠右，主操作放最右：打包中只有 Hide（任务接着跑），成功后 Show in Finder */
		const char* primary_label = "Close";
		const char* secondary_label = nullptr;
		bool primary_accent = true;

		switch (model.Mode)
		{
		case PackageDialogMode::Progress: primary_label = "Hide"; primary_accent = false; break;
		case PackageDialogMode::Success: primary_label = "Show in Finder"; secondary_label = "Close"; break;
		default: break;
		}

		/* 主次按钮等宽：两枚按钮尺寸不齐时右端会显得参差 */
		float text_width = ImGui::CalcTextSize(primary_label).x;
		if (secondary_label != nullptr)
			text_width = std::max(text_width, ImGui::CalcTextSize(secondary_label).x);

		const float button_w = text_width + style.FramePadding.x * 2.0f + Scaled(12.0f);
		const int button_count = (secondary_label != nullptr) ? 2 : 1;
		const float row_width = button_w * static_cast<float>(button_count)
			+ style.ItemSpacing.x * static_cast<float>(button_count - 1);
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, content_width - row_width));

		bool close = false;
		if (secondary_label != nullptr)
		{
			close = ImGui::Button(secondary_label, ImVec2(button_w, 0.0f));
			ImGui::SameLine();
		}

		if (primary_accent)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Token::Accent);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::Token::AccentHover);
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::Token::AccentDown);
			ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::Neutral0);
		}

		const bool primary = ImGui::Button(primary_label, ImVec2(button_w, 0.0f));

		if (primary_accent)
			ImGui::PopStyleColor(4);

		if (ImGui::IsWindowAppearing())
			ImGui::SetItemDefaultFocus();

		const bool reveal = primary && model.Mode == PackageDialogMode::Success;
		if (close || primary || KeyPressed(ImGuiKey_Escape))
			ImGui::CloseCurrentPopup();

		ImGui::EndPopup();
		return reveal;
	}
}
