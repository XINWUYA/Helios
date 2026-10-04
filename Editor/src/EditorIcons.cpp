#include "Pch.h"
#include "EditorIcons.h"
#include "Helios/ImGui/EditorTheme.h"

namespace Helios::Icons
{
	namespace
	{
		/* 与 Assets/EditorRes/icons/ 下的 PNG 一一对应 */
		constexpr IconDesc s_Icons[] = {
			{ "None",         nullptr,                             nullptr },
			{ "Save",         "EditorRes/icons/save.png",          nullptr },
			{ "Play",         "EditorRes/icons/play.png",          nullptr },
			{ "Stop",         "EditorRes/icons/stop.png",          nullptr },
			{ "Translate",    "EditorRes/icons/translate.png",     nullptr },
			{ "Rotate",       "EditorRes/icons/rotate.png",        nullptr },
			{ "Scale",        "EditorRes/icons/scale.png",         nullptr },
			{ "Menu",         "EditorRes/icons/menu.png",          nullptr },
			{ "Add",          "EditorRes/icons/add.png",           nullptr },
			{ "Return",       "EditorRes/icons/return.png",        nullptr },
			{ "Filter",       "EditorRes/icons/filter.png",        nullptr },
			{ "Directory",    "EditorRes/icons/directory.png",     nullptr },
			{ "File",         "EditorRes/icons/file.png",          nullptr },
			{ "FileScene",    "EditorRes/icons/file_scn.png",      nullptr },
			{ "FileMtlGraph", "EditorRes/icons/file_mtlgraph.png", nullptr },
			{ "Visible",      "EditorRes/icons/visible.png",       nullptr },
		};

		static_assert(sizeof(s_Icons) / sizeof(s_Icons[0]) == static_cast<size_t>(Id::COUNT),
			"Icons 元数据表与 Id 枚举数量不一致，请同步新增项");
	}

	const IconDesc& Get(Id id)
	{
		const auto index = static_cast<size_t>(id);
		return s_Icons[index < static_cast<size_t>(Id::COUNT) ? index : 0];
	}

	std::shared_ptr<DeviceTexture> GetTexture(Id id)
	{
		/* 集中缓存：同一图标全局只加载一次 */
		static std::shared_ptr<DeviceTexture> s_Cache[static_cast<size_t>(Id::COUNT)];

		const auto index = static_cast<size_t>(id);
		if (index >= static_cast<size_t>(Id::COUNT))
			return nullptr;

		auto& slot = s_Cache[index];
		const char* png_path = s_Icons[index].PngPath;
		if (!slot && png_path != nullptr)
			slot = TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH(png_path));

		return slot;
	}

	bool IconButton(Id id, const ImVec2& size, bool checked, const char* tooltip)
	{
		const auto texture = GetTexture(id);
		if (!texture)
			return false;

		/* checked 用强调色浸染表达"选中"；hover / 按下由主题的 Button 色提供 */
		if (checked)
			ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::WithAlpha(EditorTheme::Token::Accent, 0.32f));

		const bool clicked = ImGui::ImageButton((ImTextureID)texture.get(), size, ImVec2(0, 1), ImVec2(1, 0));

		if (checked)
			ImGui::PopStyleColor();

		if (tooltip != nullptr && ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", tooltip);

		return clicked;
	}
}
