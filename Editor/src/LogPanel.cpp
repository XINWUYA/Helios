#include "Pch.h"
#include "LogPanel.h"
#include <cctype>
#include <cstdio>
#include <ctime>
#include "EditorIcons.h"
#include "PanelRegistry.h"
#include "Helios/Core/Logger.h"
#include "Helios/ImGui/EditorTheme.h"

namespace Helios
{
	namespace
	{
		/* 列间距与行高：行 = 字体高 + 4px（单行文字竖直居中） */
		constexpr float kColumnGap = 8.0f;
		constexpr float kRowPadY = 2.0f;
		constexpr float kChipGap = 6.0f;

		/* 工具行切换芯片：圆角底 + 状态圆点 + 文字（激活 = 强调色淡底，
		 * 圆点按调用方给的状态色）；返回本帧是否被点击，值由调用方翻转。
		 * 与 Frame Graph 的切换芯片同一套形状语言（同高 / 同底 / 悬停提亮 / 1px 描边）。 */
		bool ToggleChip(const char* id, const char* label, bool active, const ImVec4& dot_on)
		{
			const ImGuiStyle& style = ImGui::GetStyle();
			const float height = ImGui::GetFrameHeight();
			const ImVec2 label_size = ImGui::CalcTextSize(label);
			const float dot_radius = 3.0f;
			const float width = style.FramePadding.x * 2.0f + dot_radius * 2.0f + kChipGap + label_size.x;
			const ImVec2 pos = ImGui::GetCursorScreenPos();

			ImGui::PushID(id);
			const bool clicked = ImGui::InvisibleButton("##chip", ImVec2(width, height));
			const bool hovered = ImGui::IsItemHovered();
			ImGui::PopID();

			ImDrawList* draw_list = ImGui::GetWindowDrawList();
			draw_list->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height),
				ImGui::GetColorU32(active
					? EditorTheme::WithAlpha(EditorTheme::Token::Accent, hovered ? 0.26f : 0.17f)
					: (hovered ? EditorTheme::Token::Neutral5 : EditorTheme::Token::Neutral4)),
				style.FrameRounding);
			draw_list->AddRect(pos, ImVec2(pos.x + width, pos.y + height),
				ImGui::GetColorU32(active
					? EditorTheme::WithAlpha(EditorTheme::Token::Accent, 0.65f)
					: EditorTheme::Token::Border),
				style.FrameRounding, 0, 1.0f);

			const ImVec2 dot_center(pos.x + style.FramePadding.x + dot_radius, pos.y + height * 0.5f);
			draw_list->AddCircleFilled(dot_center, dot_radius,
				ImGui::GetColorU32(active ? dot_on : EditorTheme::WithAlpha(dot_on, 0.35f)));
			draw_list->AddText(
				ImVec2(dot_center.x + dot_radius + kChipGap, pos.y + (height - label_size.y) * 0.5f),
				ImGui::GetColorU32(active ? EditorTheme::Token::Text : EditorTheme::Token::TextDim),
				label);

			return clicked;
		}

		std::string LowercaseAscii(const char* text)
		{
			std::string lowered(text != nullptr ? text : "");
			for (char& character : lowered)
				character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
			return lowered;
		}

		/* 不区分大小写的子串查找（needle 需已小写化；逐字节比较对 UTF-8 中文同样有效——
		 * 高位字节经 tolower 原样保留，两侧编码一致即可命中） */
		bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle_lower)
		{
			if (needle_lower.empty())
				return true;
			if (haystack.size() < needle_lower.size())
				return false;

			for (size_t start = 0; start + needle_lower.size() <= haystack.size(); ++start)
			{
				size_t matched = 0;
				while (matched < needle_lower.size()
					&& static_cast<char>(std::tolower(static_cast<unsigned char>(haystack[start + matched])))
						== needle_lower[matched])
				{
					++matched;
				}
				if (matched == needle_lower.size())
					return true;
			}
			return false;
		}

		void FormatTime(std::time_t seconds, uint32_t millis, char (&out)[32])
		{
			std::tm local_time{};
#ifdef _WIN32
			localtime_s(&local_time, &seconds);
#else
			localtime_r(&seconds, &local_time);
#endif
			std::snprintf(out, sizeof(out), "%02d:%02d:%02d.%03u",
				local_time.tm_hour, local_time.tm_min, local_time.tm_sec, millis);
		}

		/* 级别标：短标签（列宽按最长的 "ERROR" 定） */
		const char* LevelTag(int level)
		{
			switch (level)
			{
			case spdlog::level::trace:    return "TRACE";
			case spdlog::level::debug:    return "DEBUG";
			case spdlog::level::info:     return "INFO";
			case spdlog::level::warn:     return "WARN";
			case spdlog::level::err:      return "ERROR";
			case spdlog::level::critical: return "CRIT";
			default:                      return "-";
			}
		}

		/* 级别标色：正文由该级别的语义色着色（TRACE 弱 / DEBUG 次要 / INFO 正文 /
		 * WARN 暖黄 / ERROR+CRIT 错误色），时间与来源列走次要色 */
		const ImVec4& LevelColor(int level)
		{
			switch (level)
			{
			case spdlog::level::trace:    return EditorTheme::Token::TextDim;
			case spdlog::level::debug:    return EditorTheme::Token::TextLabel;
			case spdlog::level::warn:     return EditorTheme::Token::Warning;
			case spdlog::level::err:
			case spdlog::level::critical: return EditorTheme::Token::Danger;
			default:                      return EditorTheme::Token::Text;
			}
		}

		/* 行内单行化：换行等控制字符压成空格（完整原文留给悬停提示） */
		void MakeSingleLine(std::string& text)
		{
			for (char& character : text)
			{
				if (static_cast<unsigned char>(character) < 0x20 || character == 0x7f)
					character = ' ';
			}
		}
	}

	void LogPanel::OnImGuiRender(bool& visible)
	{
		PROFILE_FUNCTION();

		/* 隐藏时不提交 Begin（本仓 ImGui 的 p_open 只做写回不做隐藏；
		 * 恢复入口在 View 菜单，与 Frame Graph 同一约定） */
		if (!visible)
			return;

		ImGui::SetNextWindowSize(ImVec2(620.0f, 320.0f), ImGuiCond_FirstUseEver);

		if (!ImGui::Begin(Panel::kLog, &visible))
		{
			ImGui::End();
			return;
		}

		SyncEntries();
		RebuildVisibleEntries();

		/* 面板标题由页签承担（"Log"）；面板内不重复标题行，清空动作在工具行右端（见 ShowToolbar） */
		ShowToolbar();
		ShowEntries();

		ImGui::End();
	}

	void LogPanel::SetSearchFilter(const std::string& filter)
	{
		m_SearchFilter = LowercaseAscii(filter.c_str());
		std::snprintf(m_SearchBuffer, sizeof(m_SearchBuffer), "%s", filter.c_str());
	}

	void LogPanel::SetLevelVisible(spdlog::level::level_enum level, bool visible)
	{
		const auto index = static_cast<size_t>(level);
		if (index < IM_ARRAYSIZE(m_LevelVisible))
			m_LevelVisible[index] = visible;
	}

	bool LogPanel::IsLevelVisible(spdlog::level::level_enum level) const
	{
		const auto index = static_cast<size_t>(level);
		return index < IM_ARRAYSIZE(m_LevelVisible) ? m_LevelVisible[index] : true;
	}

	void LogPanel::SyncEntries()
	{
		std::vector<LogBuffer::Record> fetched;
		const bool rebuild = LogBuffer::Instance().FetchSince(m_LastSeq, fetched);

		if (rebuild)
			m_Entries.clear();

		for (LogBuffer::Record& record : fetched)
			m_Entries.push_back(std::move(record));

		/* 缓存与环形缓冲同容量截断（显示缓存不需要比数据源留得更旧） */
		if (m_Entries.size() > LogBuffer::kCapacity)
			m_Entries.erase(m_Entries.begin(), m_Entries.begin() + (m_Entries.size() - LogBuffer::kCapacity));
	}

	void LogPanel::RebuildVisibleEntries()
	{
		m_VisibleEntries.clear();
		m_VisibleEntries.reserve(m_Entries.size());

		for (uint32_t index = 0; index < m_Entries.size(); ++index)
		{
			if (PassesFilter(m_Entries[index]))
				m_VisibleEntries.push_back(index);
		}
	}

	bool LogPanel::PassesFilter(const LogBuffer::Record& record) const
	{
		if (!IsLevelVisible(static_cast<spdlog::level::level_enum>(record.Level)))
			return false;

		if (m_SearchFilter.empty())
			return true;

		return ContainsCaseInsensitive(record.Message, m_SearchFilter)
			|| ContainsCaseInsensitive(record.Logger, m_SearchFilter);
	}

	void LogPanel::ShowToolbar()
	{
		const ImGuiStyle& style = ImGui::GetStyle();

		/* 级别过滤芯片：圆点着色 = 该级别的标色（未选整体压暗）；
		 * Error 芯片连 Critical 一起开关（两者同一显示语义） */
		const auto level_chip = [&](const char* label, spdlog::level::level_enum level,
			const ImVec4& color, bool with_critical)
		{
			const auto index = static_cast<size_t>(level);
			if (ToggleChip(label, label, m_LevelVisible[index], color))
			{
				m_LevelVisible[index] = !m_LevelVisible[index];
				if (with_critical)
					m_LevelVisible[static_cast<size_t>(spdlog::level::critical)] = m_LevelVisible[index];
			}
		};

		level_chip("Trace", spdlog::level::trace, EditorTheme::Token::TextDim, false);
		ImGui::SameLine(0.0f, kChipGap);
		level_chip("Debug", spdlog::level::debug, EditorTheme::Token::TextLabel, false);
		ImGui::SameLine(0.0f, kChipGap);
		level_chip("Info", spdlog::level::info, EditorTheme::Token::Text, false);
		ImGui::SameLine(0.0f, kChipGap);
		level_chip("Warn", spdlog::level::warn, EditorTheme::Token::Warning, false);
		ImGui::SameLine(0.0f, kChipGap);
		level_chip("Error", spdlog::level::err, EditorTheme::Token::Danger, true);

		/* 右端：搜索框 + 跟随开关 + 计数 + 清空（贴行右缘；空间不足时压缩搜索框） */
		char count_text[48];
		std::snprintf(count_text, sizeof(count_text), "%u / %u",
			GetVisibleCount(), GetEntryCount());
		const float count_width = ImGui::CalcTextSize(count_text).x;
		const float follow_width = style.FramePadding.x * 2.0f + 3.0f * 2.0f + kChipGap
			+ ImGui::CalcTextSize("Follow").x;
		const float clear_width = ImGui::GetFrameHeight(); /* 清空按钮：行高见方 */

		const float left_end = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
		const float content_max = ImGui::GetContentRegionMax().x;
		const float search_width = std::clamp(
			content_max - left_end - style.ItemSpacing.x * 4.0f - follow_width - count_width - clear_width,
			90.0f, 170.0f);
		const float right_width = search_width + style.ItemSpacing.x * 3.0f + follow_width + count_width + clear_width;
		const float right_start = std::max(left_end + style.ItemSpacing.x, content_max - right_width);
		ImGui::SameLine(right_start);

		Icons::BeginSearchInput();
		ImGui::SetNextItemWidth(search_width);
		if (ImGui::InputTextWithHint("##LogFind", "Search...", m_SearchBuffer, IM_ARRAYSIZE(m_SearchBuffer)))
			m_SearchFilter = LowercaseAscii(m_SearchBuffer);
		Icons::EndSearchInput();

		ImGui::SameLine();
		if (ToggleChip("Follow", "Follow", m_Follow, EditorTheme::Token::Accent))
			m_Follow = !m_Follow;

		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ImGui::TextColored(EditorTheme::Token::TextDim, "%s", count_text);

		/* 清空：缓冲与显示缓存一起清 */
		ImGui::SameLine();
		if (Icons::IconButton(Icons::Id::Remove, ImVec2(clear_width, clear_width), false, "Clear Log"))
		{
			LogBuffer::Instance().Clear();
			m_Entries.clear();
			m_VisibleEntries.clear();
		}

		ImGui::Dummy(ImVec2(0.0f, 2.0f));
	}

	void LogPanel::ShowEntries()
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const float row_height = ImGui::GetTextLineHeight() + kRowPadY * 2.0f;
		const float row_pitch = row_height + style.ItemSpacing.y;

		ImGui::BeginChild("##LogEntries", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_None);
		{
			/* ListClipper 虚拟化：只画视口内的行（缓冲上限 2048，全画也不致命，
			 * 但查询 / 反复打开时逐行布局是纯浪费） */
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(m_VisibleEntries.size()), row_pitch);
			while (clipper.Step())
			{
				for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
					DrawEntryRow(m_Entries[m_VisibleEntries[static_cast<size_t>(row)]], row_height);
			}
			clipper.End();

			if (m_VisibleEntries.empty())
			{
				ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
				ImGui::TextUnformatted(m_Entries.empty()
					? "No log entries."
					: "No entries match the current filter.");
				ImGui::PopStyleColor();
			}

			/* 跟随语义：手动上滚自动暂停（先看历史），滚回底部恢复 */
			if (ImGui::IsWindowHovered())
			{
				const float wheel = ImGui::GetIO().MouseWheel;
				if (wheel > 0.0f && m_Follow)
					m_Follow = false;
				else if (wheel < 0.0f && !m_Follow
					&& ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 0.5f)
					m_Follow = true;
			}

			if (m_Follow)
				ImGui::SetScrollHereY(1.0f);
		}
		ImGui::EndChild();
	}

	void LogPanel::DrawEntryRow(const LogBuffer::Record& record, float row_height)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const ImVec2 row_min = ImGui::GetCursorScreenPos();
		const float row_width = ImGui::GetContentRegionAvail().x;

		/* 行命中区：悬停底色（Selectable 自带）；双击 = 复制该条 */
		ImGui::PushID(static_cast<int>(record.Seq & 0x7fffffff));
		ImGui::Selectable("##LogRow", false, ImGuiSelectableFlags_None, ImVec2(row_width, row_height));
		const bool hovered = ImGui::IsItemHovered();
		ImGui::PopID();

		char time_text[32];
		FormatTime(record.Seconds, record.Millis, time_text);

		if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		{
			const std::string line = std::string(time_text) + " [" + LevelTag(record.Level) + "] "
				+ record.Logger + ": " + record.Message;
			ImGui::SetClipboardText(line.c_str());
		}

		/* 列布局（定宽列让时间 / 级别 / 来源逐行对齐；正文占余下宽度） */
		ImFont* font = ImGui::GetFont();
		const float font_size = ImGui::GetFontSize();
		const float text_y = row_min.y + (row_height - font_size) * 0.5f;
		ImDrawList* draw_list = ImGui::GetWindowDrawList();

		float column_x = row_min.x + style.FramePadding.x;

		const float time_width = ImGui::CalcTextSize("00:00:00.000").x;
		draw_list->AddText(font, font_size, ImVec2(column_x, text_y),
			ImGui::GetColorU32(EditorTheme::Token::TextDim), time_text);
		column_x += time_width + kColumnGap;

		const float level_width = ImGui::CalcTextSize("ERROR").x;
		draw_list->AddText(font, font_size, ImVec2(column_x, text_y),
			ImGui::GetColorU32(LevelColor(record.Level)), LevelTag(record.Level));
		column_x += level_width + kColumnGap;

		const float logger_width = ImGui::CalcTextSize("Kernel").x;
		draw_list->AddText(font, font_size, ImVec2(column_x, text_y),
			ImGui::GetColorU32(EditorTheme::Token::TextDim), record.Logger.c_str());
		column_x += logger_width + kColumnGap;

		/* 正文：单行化后画；超宽省略号收尾（完整原文在悬停提示里） */
		m_RowText = record.Message;
		MakeSingleLine(m_RowText);
		const float message_width = ImMax(row_min.x + row_width - style.FramePadding.x - column_x, 1.0f);

		const ImVec2 message_size = ImGui::CalcTextSize(m_RowText.c_str());
		const ImU32 message_color = ImGui::GetColorU32(LevelColor(record.Level));
		if (message_size.x <= message_width)
		{
			draw_list->AddText(font, font_size, ImVec2(column_x, text_y), message_color, m_RowText.c_str());
		}
		else
		{
			ImGui::RenderTextEllipsis(draw_list, ImVec2(column_x, text_y),
				ImVec2(column_x + message_width, text_y + ImGui::GetTextLineHeight()),
				column_x + message_width, column_x + message_width,
				m_RowText.c_str(), nullptr, &message_size);
		}

		/* 悬停：完整原文 + 复制提示 */
		if (hovered)
		{
			ImGui::BeginTooltip();
			ImGui::TextColored(LevelColor(record.Level), "%s  %s  (%s)",
				time_text, LevelTag(record.Level), record.Logger.c_str());
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
			ImGui::TextUnformatted(record.Message.c_str());
			ImGui::PopTextWrapPos();
			ImGui::TextDisabled("Double-click to copy");
			ImGui::EndTooltip();
		}
	}
}
