#include "Pch.h"
#include "BuiltinTextures.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include "Command/AssetFileOps.h"
#include "UI/PanelChrome.h"
#include "Helios/Common/AssetPath.h"
#include "Helios/Common/PathUtils.h"
#include "Helios/Core/AssetManager.h"
#include "Helios/VirtualDevice/DeviceTexture.h"

namespace Helios::BuiltinTextures
{
	namespace
	{
		constexpr const char* kPopupId = "##BuiltinTextureMenu";
		constexpr const char* kSearchId = "##Search";

		/* 弹层自己的状态：每次重开都从空搜索框起手 */
		char s_Search[64] = {};
		bool s_SearchWasOpen = false;
		bool s_FocusSearch = false;

		/* 预览图跟着清单走：弹层开着才取，没起图形设备时不会去碰纹理解码。
		 * 内置图也按项目根优先解析（项目里放同名文件即覆盖），换工程要重取一遍。 */
		const std::vector<SharedPtr<DeviceTexture>>& Previews()
		{
			static std::vector<SharedPtr<DeviceTexture>> s_Previews;
			static uint32_t s_Generation = 0;

			const uint32_t generation = GetProjectPathGeneration();
			if (!s_Previews.empty() && s_Generation == generation)
				return s_Previews;

			s_Previews.clear();
			s_Generation = generation;
			for (const Entry& entry : List())
			{
				const std::filesystem::path path = ResolveAssetPath(PathFromUtf8(entry.RelativePath));
				s_Previews.push_back(AssetManager::Instance().GetOrLoad<DeviceTexture>(PathToUtf8(path)));
			}
			return s_Previews;
		}

		std::string Lower(std::string text)
		{
			for (char& character : text)
				character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
			return text;
		}

		/* 一条内置贴图：左边预览图，右边名字，选中的那条右缘打勾 */
		bool TextureRow(const Entry& entry, const SharedPtr<DeviceTexture>& preview, bool selected, float width)
		{
			const ImGuiStyle& style = ImGui::GetStyle();
			const float row_height = ImGui::GetFrameHeight();
			const float preview_size = row_height - 4.0f;
			const float gap = style.ItemInnerSpacing.x;

			ImGui::PushID(entry.Name.c_str());
			const bool clicked = ImGui::Selectable("##row", false,
				ImGuiSelectableFlags_SelectOnRelease | ImGuiSelectableFlags_DontClosePopups
					| ImGuiSelectableFlags_SpanAvailWidth, ImVec2(width, row_height));
			ImGui::PopID();

			const ImVec2 min = ImGui::GetItemRectMin();
			const ImVec2 max = ImGui::GetItemRectMax();
			const float center_y = (min.y + max.y) * 0.5f;
			ImDrawList* const draw = ImGui::GetWindowDrawList();

			/* 预览跟贴图槽里的缩略图同一个翻法（贴图加载时已经翻过 V） */
			const ImVec2 preview_min(min.x + style.FramePadding.x, center_y - preview_size * 0.5f);
			const ImVec2 preview_max(preview_min.x + preview_size, preview_min.y + preview_size);
			if (preview != nullptr && preview->IsLoaded())
				draw->AddImage((ImTextureID)preview.get(), preview_min, preview_max, ImVec2(0, 1), ImVec2(1, 0));
			draw->AddRect(preview_min, preview_max, ImGui::GetColorU32(ImGuiCol_Border));

			draw->AddText(ImVec2(preview_max.x + gap, center_y - ImGui::GetFontSize() * 0.5f),
				ImGui::GetColorU32(EditorTheme::Token::Text), entry.Name.c_str());

			if (selected)
				PanelChrome::DrawMenuRowCheck(draw, max, center_y, ImGui::GetFontSize());

			return clicked;
		}
	}

	const std::vector<Entry>& List()
	{
		static const std::vector<Entry> s_List = []
		{
			std::vector<Entry> entries;

			std::error_code error;
			const std::filesystem::path root = g_EngineResPath / "Textures";
			for (const auto& item : std::filesystem::directory_iterator(root, error))
			{
				const std::filesystem::path& path = item.path();
				/* 只收图片：.texmeta 这类伴生文件不算贴图 */
				if (!item.is_regular_file(error)
					|| AssetFileKindOf(PathToUtf8(path.extension())) != AssetFileKind::Image)
					continue;

				entries.push_back({ PathToUtf8(path.stem()), PathToUtf8(path.lexically_relative(g_EngineResPath)) });
			}

			std::sort(entries.begin(), entries.end(),
				[](const Entry& lhs, const Entry& rhs) { return lhs.Name < rhs.Name; });
			return entries;
		}();

		return s_List;
	}

	bool Contains(const std::string& relative_path)
	{
		if (relative_path.empty())
			return false;

		const std::vector<Entry>& entries = List();
		return std::any_of(entries.begin(), entries.end(),
			[&relative_path](const Entry& entry) { return entry.RelativePath == relative_path; });
	}

	bool DrawPicker(const char* id, const std::string& current, std::string& picked)
	{
		const std::vector<Entry>& entries = List();
		if (entries.empty())
			return false;

		const ImGuiStyle& style = ImGui::GetStyle();
		const float size = ImGui::GetFrameHeight();
		const float arrow = ImGui::GetFontSize() * 0.55f;

		ImGui::PushID(id);

		/* 按钮只画一枚朝下的箭头，和 Shader 下拉同一个语言 */
		const bool open = ImGui::IsPopupOpen(kPopupId);
		if (open)
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
		const bool clicked = ImGui::Button("##Arrow", ImVec2(size, size));
		if (open)
			ImGui::PopStyleColor();

		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		PanelChrome::DrawDisclosureArrow(ImGui::GetWindowDrawList(),
			ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f), arrow, true,
			ImGui::GetColorU32(EditorTheme::Token::TextDim));

		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Pick a built-in texture");
		if (clicked)
			ImGui::OpenPopup(kPopupId);

		bool picked_any = false;
		/* 弹层版式和别的菜单同一套，样式必须压在 BeginPopup 之前 */
		PanelChrome::PushMenuStyle();
		if (ImGui::BeginPopup(kPopupId))
		{
			if (!s_SearchWasOpen)
			{
				s_Search[0] = '\0';
				s_FocusSearch = true;
			}
			s_SearchWasOpen = true;

			/* 宽度先按最长的名字定下来：搜索框和各行走同一个宽度，筛到只剩一条也不忽宽忽窄 */
			const float preview_size = ImGui::GetFrameHeight() - 4.0f;
			const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
			float row_width = 0.0f;
			for (const Entry& entry : entries)
				row_width = ImMax(row_width, style.FramePadding.x * 2.0f + preview_size + gap
					+ ImGui::CalcTextSize(entry.Name.c_str()).x + gap + ImGui::GetFontSize());

			ImGui::SetNextItemWidth(row_width);
			if (s_FocusSearch)
			{
				ImGui::SetKeyboardFocusHere();
				s_FocusSearch = false;
			}
			Icons::BeginSearchInput();
			ImGui::InputTextWithHint(kSearchId, "Search", s_Search, IM_ARRAYSIZE(s_Search));
			Icons::EndSearchInput();

			const std::string filter = Lower(s_Search);
			PanelChrome::MenuSectionHeader("Built-in");

			const std::vector<SharedPtr<DeviceTexture>>& previews = Previews();
			size_t shown = 0;
			for (size_t index = 0; index < entries.size(); ++index)
			{
				const Entry& entry = entries[index];
				if (!filter.empty() && Lower(entry.Name).find(filter) == std::string::npos)
					continue;

				++shown;
				if (TextureRow(entry, previews[index], entry.RelativePath == current, row_width))
				{
					picked = entry.RelativePath;
					picked_any = true;
					ImGui::CloseCurrentPopup();
				}
			}

			if (shown == 0)
				PanelChrome::MenuStatusText("No match");

			ImGui::EndPopup();
		}
		else
		{
			s_SearchWasOpen = false;
		}
		PanelChrome::PopMenuStyle();

		ImGui::PopID();
		return picked_any;
	}
}
