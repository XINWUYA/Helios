#pragma once
#include <imgui.h>

#include "EditorUI/EditorIcons.h"
#include "EditorUI/EditorTheme.h"

namespace Helios
{
	/* 滑动页签：两个 / 三个等宽格共用一条底槽，选中块平滑滑过去。
	 * 返回本帧被点击的格下标（-1 = 未点击）；slide 由调用方持久化。 */
	namespace Widgets
	{
		int SlidingTabPair(const char* id, const char* label_left, Icons::Id icon_left,
			const char* label_right, Icons::Id icon_right, bool right_selected, float& slide,
			float width_override = 0.0f);

		int SlidingTabTriple(const char* id,
			const char* label0, Icons::Id icon0, const char* label1, Icons::Id icon1,
			const char* label2, Icons::Id icon2, int selected, float& slide,
			float width_override = 0.0f);

		/* 两段开关的紧凑宽度：每格 = 图标 + 间隔 + 最宽标签 + 两侧留白 */
		float SlidingTabPairWidth(const char* label_left, const char* label_right, bool with_icon);

		/* 三格切换器的紧凑宽度：三段各 = 行高 + 8 */
		float TimingSwitcherWidth();
	}
}
