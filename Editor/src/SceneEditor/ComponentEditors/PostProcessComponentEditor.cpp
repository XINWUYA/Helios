#include "Pch.h"
#include "ComponentEditors.h"
#include "ImGui/EditorTheme.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include "Helios/Scene/Components.h"

namespace Helios
{
	namespace
	{
		/* 后处理对象卡：总开关走 schema 字段（卡片顶部），这里补一行提示讲清"效果是独立组件"的模型
		 * （空卡片没有出路 —— 用户找不到 Bloom 在哪加）。 */
		bool DrawPostProcessBlock(void* raw)
		{
			(void)raw;
			ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
			ImGui::TextWrapped("Effects are separate components - add them via the + menu (e.g. Bloom).");
			ImGui::PopStyleColor();
			return false;
		}
	}

	void RegisterPostProcessComponentEditor()
	{
		ComponentRegistry::Instance().SetCustomDraw(typeid(PostProcessComponent), &DrawPostProcessBlock);
	}
}
