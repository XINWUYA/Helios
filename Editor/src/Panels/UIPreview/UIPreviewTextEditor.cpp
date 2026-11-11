#include "Pch.h"
#include "UIPreviewTextEditor.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

#include "Helios/Application/ProjectConfig.h"
#include "CssPatch.h"
#include "EditorUI/EditorIcons.h"
#include "EditorUI/EditorTheme.h"
#include "EditorUI/Widgets/SlidingTabs.h"

namespace Helios::UIEditor
{
	namespace
	{
		/* InputTextMultiline 直接编辑 std::string：按需要扩缓冲（imgui_stdlib 的做法） */
		int TextResizeCallback(ImGuiInputTextCallbackData* data)
		{
			if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
			{
				auto* text = static_cast<std::string*>(data->UserData);
				text->resize(static_cast<size_t>(data->BufTextLen));
				data->Buf = text->data();
			}
			return 0;
		}
	}

	void UIPreviewTextEditor::SetDocument(const std::string& document_path)
	{
		m_DocumentPath = document_path;
		m_EditingStyle = false;
		Load();
	}

	std::string UIPreviewTextEditor::StylePath() const
	{
		if (m_DocumentPath.empty())
			return {};

		std::filesystem::path style(m_DocumentPath);
		style.replace_extension(".rcss");

		std::error_code error;
		if (!std::filesystem::exists(g_ProjectPath / style, error))
			return {};
		return style.generic_string();
	}

	std::string UIPreviewTextEditor::CurrentPath() const
	{
		return m_EditingStyle ? StylePath() : m_DocumentPath;
	}

	void UIPreviewTextEditor::Load()
	{
		const std::string path = CurrentPath();
		if (path.empty())
			return;

		std::ifstream file(g_ProjectPath / path, std::ios::binary);
		if (!file)
			return;

		std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		m_Text = text;
		m_TextSaved = std::move(text);

		std::error_code error;
		m_TextStamp = std::filesystem::last_write_time(g_ProjectPath / path, error);
	}

	void UIPreviewTextEditor::ReloadIfClean()
	{
		if (!IsDirty())
			Load();
	}

	std::string UIPreviewTextEditor::PatchText(std::string source, const std::string& id,
		const std::vector<std::pair<std::string, std::string>>& declarations, int& written)
	{
		bool found = true;
		written = 0;
		for (const auto& [property, value] : declarations)
		{
			if (PatchCssDeclaration(source, id, property, value, found))
				++written;
		}

		if (!found)
		{
			written = -1;
			return {};
		}
		return source;
	}

	void UIPreviewTextEditor::SetText(const std::string& text)
	{
		m_Text = text;
		m_TextSaved = text;

		const std::string path = CurrentPath();
		if (path.empty())
			return;

		std::error_code error;
		m_TextStamp = std::filesystem::last_write_time(g_ProjectPath / path, error);
	}

	void UIPreviewTextEditor::Draw(const ImVec2& position, const ImVec2& size)
	{
		ImGui::SetCursorScreenPos(position);
		ImGui::BeginChild("##ui-preview-text", size, true);
		DrawToolbar();
		ImGui::Separator();
		DrawSource();
		ImGui::EndChild();
	}

	void UIPreviewTextEditor::DrawToolbar()
	{
		const std::string style_path = StylePath();
		const float pair_width = Widgets::SlidingTabPairWidth("Document", "Style", true);
		ImGui::SameLine(std::max(0.0f, ImGui::GetWindowContentRegionMax().x - pair_width));
		const int picked = Widgets::SlidingTabPair("##ui-preview-kind", "Document", Icons::Id::FileUI,
			"Style", Icons::Id::File, m_EditingStyle, m_KindSlide, pair_width);
		if (picked == 0 && m_EditingStyle)
		{
			m_EditingStyle = false;
			Load();
		}
		else if (picked == 1 && !m_EditingStyle && !style_path.empty())
		{
			m_EditingStyle = true;
			Load();
		}
	}

	void UIPreviewTextEditor::DrawSource()
	{
		/* 行号槽：文本放在一个自带滚动的子窗里，行号与文本按同一个行高排，
		 * 滚动时一起动（滚动量取上一帧的值，拖滚动条时行号晚一帧跟上） */
		int line_count = 1;
		for (const char character : m_Text)
		{
			if (character == '\n')
				++line_count;
		}
		int gutter_digits = 1;
		for (int value = line_count; value >= 10; value /= 10)
			++gutter_digits;

		const float line_height = ImGui::GetTextLineHeight();
		const float gutter_width = ImGui::CalcTextSize("0").x * static_cast<float>(gutter_digits)
			+ ImGui::GetStyle().FramePadding.x * 2.0f;
		const float content_height = line_height * static_cast<float>(line_count + 1);
		const float visible_height = ImGui::GetContentRegionAvail().y;
		const float text_height = std::max(content_height, visible_height - line_height * 0.5f);

		/* 整块代码区一个底色：输入框不再自己画一层框，行号与正文同底，
		 * 看起来才是一个编辑器而不是「行号 + 一个框」 */
		const ImVec4 editor_surface = EditorTheme::Token::Neutral0;
		ImGui::PushStyleColor(ImGuiCol_ChildBg, editor_surface);
		ImGui::PushStyleColor(ImGuiCol_FrameBg, editor_surface);
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, editor_surface);
		ImGui::PushStyleColor(ImGuiCol_FrameBgActive, editor_surface);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);

		ImGui::BeginChild("##ui-preview-code", ImVec2(0.0f, 0.0f), false);
		{
			ImDrawList* code_draw = ImGui::GetWindowDrawList();
			/* GetCursorScreenPos 里已经减过滚动量，行号再加一遍就是重复偏移，
			 * 表现为行号比正文跑得快（滚动越多差得越远） */
			const ImVec2 content_top = ImGui::GetCursorScreenPos();

			/* 行号右对齐，颜色压暗一档；首行要跟输入框的内边距对齐 */
			const ImU32 number_color = ImGui::GetColorU32(EditorTheme::Token::Neutral7);
			const float first_line_offset = ImGui::GetStyle().FramePadding.y;
			for (int line = 0; line < line_count; ++line)
			{
				char number[16];
				std::snprintf(number, sizeof(number), "%d", line + 1);
				const float text_width = ImGui::CalcTextSize(number).x;
				code_draw->AddText(ImVec2(content_top.x + gutter_width
					- ImGui::GetStyle().FramePadding.x - text_width,
					content_top.y + first_line_offset + line_height * static_cast<float>(line)),
					number_color, number);
			}

			ImVec4 separator = EditorTheme::Token::Neutral7;
			separator.w = 0.25f;
			code_draw->AddLine(
				ImVec2(content_top.x + gutter_width, content_top.y),
				ImVec2(content_top.x + gutter_width, content_top.y + text_height),
				ImGui::GetColorU32(separator));

			ImGui::SetCursorScreenPos(ImVec2(content_top.x + gutter_width + ImGui::GetStyle().FramePadding.x,
				content_top.y));
			m_Text.reserve(m_Text.size() + 1024);
			ImGui::InputTextMultiline("##ui-preview-source", m_Text.data(),
				m_Text.capacity() + 1,
				ImVec2(std::max(80.0f, ImGui::GetContentRegionAvail().x - gutter_width), text_height),
				ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackResize,
				&TextResizeCallback, &m_Text);
		}
		ImGui::EndChild();

		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(4);
	}
}
