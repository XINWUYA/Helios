#pragma once
/* Debug View 的档位表与弹层行（编辑器共用一份）：场景视口的 View 菜单 / 工具栏弹层、
 * 模型编辑器预览工具的入口都从这里取 —— 分类、标签、图形只有这一处，加档位只改这里。
 * 这里只负责"画行 + 点选回调"，档位落到哪台相机（EditorContext / 模型预览相机）由调用方决定。 */

#include <cstddef>
#include <functional>
#include <imgui.h>
#include "EditorIcons.h"
#include "PanelChrome.h"
#include "Helios/Renderer/RenderCommon.h"

namespace Helios::DebugViewMenu
{
	struct Item
	{
		DebugViewMode Mode;
		Icons::Id Icon;
		const char* Label;
	};

	/* 四个分区（顺序即菜单顺序；分区名做段头） */
	inline constexpr Item kSurfaceItems[] = {
		{ DebugViewMode::Albedo, Icons::Id::Albedo, "Albedo" },
		{ DebugViewMode::Normal, Icons::Id::Normal, "Normal" },
		{ DebugViewMode::Roughness, Icons::Id::Roughness, "Roughness" },
		{ DebugViewMode::Metallic, Icons::Id::Metallic, "Metallic" },
		{ DebugViewMode::SpecularColor, Icons::Id::SpecularColor, "Specular Color" },
		{ DebugViewMode::AmbientOcclusion, Icons::Id::AmbientOcclusion, "Ambient Occlusion" },
		{ DebugViewMode::Emission, Icons::Id::Emission, "Emission" },
		{ DebugViewMode::Ambient, Icons::Id::Ambient, "Ambient" },
	};

	inline constexpr Item kLightingItems[] = {
		{ DebugViewMode::Diffuse, Icons::Id::Diffuse, "Diffuse" },
		{ DebugViewMode::Specular, Icons::Id::Specular, "Specular" },
		{ DebugViewMode::IndirectDiffuse, Icons::Id::IndirectDiffuse, "Indirect Diffuse" },
		{ DebugViewMode::IndirectSpecular, Icons::Id::IndirectSpecular, "Indirect Specular" },
		{ DebugViewMode::Shadow, Icons::Id::Shadow, "Shadow" },
		{ DebugViewMode::Probe, Icons::Id::Probe, "Probe" },
	};

	inline constexpr Item kAnalysisItems[] = {
		{ DebugViewMode::Overdraw, Icons::Id::Overdraw, "Overdraw" },
		{ DebugViewMode::Mipmap, Icons::Id::Mipmap, "Mipmap" },
		{ DebugViewMode::Wireframe, Icons::Id::Grid, "Wireframe" },
	};

	/* 骨骼节点预览：叠在正常着色上，不改成像 */
	inline constexpr Item kAnimationItems[] = {
		{ DebugViewMode::Skeleton, Icons::Id::Animation, "Skeleton" },
	};

	/* Shaded 复位行（None = 正常着色） */
	inline bool DrawResetRow(DebugViewMode current)
	{
		return PanelChrome::MenuItemSelectWithIcon(Icons::Id::DebugView, "Shaded",
			current == DebugViewMode::None);
	}

	/* 单个档位行：选中态由 current 决定，点击只回传、不自己改档 */
	inline bool DrawItemRow(DebugViewMode current, const Item& item, bool enabled = true)
	{
		return PanelChrome::MenuItemSelectWithIcon(item.Icon, item.Label, current == item.Mode, enabled);
	}

	/* 一个分区：段头 + 若干行；enabled = false 时整段置灰（如前向管线下的光照分量） */
	inline void DrawCategory(const char* title, DebugViewMode current, const Item* items, size_t count,
		bool enabled, const std::function<void(DebugViewMode)>& on_select)
	{
		PanelChrome::MenuSectionHeader(title);
		for (size_t index = 0; index < count; ++index)
		{
			if (DrawItemRow(current, items[index], enabled))
				on_select(items[index].Mode);
		}
	}

	/* 全部档位行：Shaded 复位行 + Surface / Lighting / Analysis / Animation 四段 */
	inline void DrawItems(DebugViewMode current, bool lighting_enabled,
		const std::function<void(DebugViewMode)>& on_select)
	{
		if (DrawResetRow(current))
			on_select(DebugViewMode::None);

		DrawCategory("SURFACE", current, kSurfaceItems, IM_ARRAYSIZE(kSurfaceItems), true, on_select);
		DrawCategory("LIGHTING", current, kLightingItems, IM_ARRAYSIZE(kLightingItems), lighting_enabled, on_select);
		DrawCategory("ANALYSIS", current, kAnalysisItems, IM_ARRAYSIZE(kAnalysisItems), true, on_select);
		DrawCategory("ANIMATION", current, kAnimationItems, IM_ARRAYSIZE(kAnimationItems), true, on_select);
	}

	/* 档位显示名（Shaded / 未知档返回空） */
	inline const char* ModeLabel(DebugViewMode mode)
	{
		const auto find_in = [mode](const Item* items, size_t count) -> const char*
		{
			for (size_t index = 0; index < count; ++index)
			{
				if (items[index].Mode == mode)
					return items[index].Label;
			}
			return nullptr;
		};

		if (const char* surface = find_in(kSurfaceItems, IM_ARRAYSIZE(kSurfaceItems)))
			return surface;
		if (const char* lighting = find_in(kLightingItems, IM_ARRAYSIZE(kLightingItems)))
			return lighting;
		if (const char* analysis = find_in(kAnalysisItems, IM_ARRAYSIZE(kAnalysisItems)))
			return analysis;
		return find_in(kAnimationItems, IM_ARRAYSIZE(kAnimationItems));
	}

	/* 弹层最小宽度：按最长档位名算，各行铺满后勾选列才对齐（调用方 SetNextWindowSizeConstraints 用） */
	inline float MinimumMenuWidth()
	{
		const auto widest_label = [](const Item* items, size_t count)
		{
			float width = 0.0f;
			for (size_t index = 0; index < count; ++index)
				width = ImMax(width, ImGui::CalcTextSize(items[index].Label).x);
			return width;
		};

		const float label_width = ImMax(
			ImMax(widest_label(kSurfaceItems, IM_ARRAYSIZE(kSurfaceItems)),
				widest_label(kLightingItems, IM_ARRAYSIZE(kLightingItems))),
			ImMax(widest_label(kAnalysisItems, IM_ARRAYSIZE(kAnalysisItems)),
				widest_label(kAnimationItems, IM_ARRAYSIZE(kAnimationItems))));

		const ImGuiStyle& style = ImGui::GetStyle();
		/* 左右内边距 + 图标与文字间距 + 勾选列 + 右侧余量 */
		return label_width + style.FramePadding.x * 2.0f + style.ItemInnerSpacing.x * 3.0f
			+ ImGui::GetFontSize() * 2.0f;
	}
}
