#pragma once
#include <imgui.h>
#include "EditorUI/EditorIcons.h"
#include "Helios/Common/Common.h"

namespace Helios
{
	class Entity;
	class Scene;
	class CommandStack;

	/* 层级树与属性面板共用的小工具（实现在 SceneHierarchy.cpp） */
	namespace SceneHierarchyShared
	{
		/* 实体在树里"最像什么"：按持有的组件挑图标，没有可识别组件就用通用图标 */
		Icons::Id ResolveEntityIcon(const Entity& entity);

		/* 可见性开关（眼睛）：命中区 + 悬停底色 + 图标 + tooltip；reveal_icon = true 时常驻 */
		bool DrawVisibilityToggle(const char* str_id, const ImVec2& rect_min, const ImVec2& rect_max,
			bool hidden, bool reveal_icon);

		/* 设置实体自身的可见性（层级行右端与属性面板标题栏的眼睛开关）；有命令栈时可撤销。
		 * visible 为目标状态：隐藏 = 补 Visibility 组件并置 false，显示 = 去组件（缺席 = 可见）。 */
		void SetEntityVisibility(const SharedPtr<Scene>& scene, CommandStack* command_stack,
			Entity entity, bool visible);
	}
}
