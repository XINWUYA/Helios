#pragma once
#include <imgui.h>
#include "UI/EditorIcons.h"

namespace Helios
{
	class Entity;

	/* 层级树与属性检视器共用的小工具（实现在 SceneHierarchy.cpp） */
	namespace SceneHierarchyShared
	{
		/* 实体在树里"最像什么"：按持有的组件挑图标，没有可识别组件就用通用图标 */
		Icons::Id ResolveEntityIcon(const Entity& entity);

		/* 可见性开关（眼睛）：命中区 + 悬停底色 + 图标 + tooltip；reveal_icon = true 时常驻 */
		bool DrawVisibilityToggle(const char* str_id, const ImVec2& rect_min, const ImVec2& rect_max,
			bool hidden, bool reveal_icon);
	}
}
