#include "Pch.h"
#include "Helios/Reflection/ComponentRegistration.h"
#include "Helios/ImGui/EditorTheme.h"
#include "Helios/Renderer/PostProcess/Effects/Bloom.h"
#include "Helios/Scene/Components.h"
#include <imgui.h>

namespace Helios
{
	namespace
	{
		/* 后处理对象卡：总开关走 schema 字段（卡片顶部），这里补一行提示讲清"效果是独立组件"的模型
		 * （空卡片没有出路——用户找不到 Bloom 在哪加）。 */
		bool DrawPostProcessBlock(void* raw)
		{
			(void)raw;
			ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
			ImGui::TextWrapped("Effects are separate components - add them via the + menu (e.g. Bloom).");
			ImGui::PopStyleColor();
			return false;
		}
	}

	void RegisterPostProcessComponents()
	{
		/* 后处理对象：总开关（关闭 = 整个对象不参与解析，效果设置保留）；效果设置组件挂在同一实体上 */
		MakeRegistrar<PostProcessComponent>("PostProcess")
			.Field(&PostProcessComponent::m_Enabled, "Enabled", FieldType::Bool)
			.CustomDraw(&DrawPostProcessBlock)
			.Register();

		/* Bloom 效果设置：关闭态只写 Enabled（其余字段由条件门控不写出） */
		MakeRegistrar<BloomComponent>("Bloom")
			.Field(&BloomComponent::m_Enabled, "Enabled", FieldType::Bool)
			.Field(&BloomComponent::m_Threshold, "Threshold", FieldType::Float, 0.01f)
			.WhenTrue(&BloomComponent::m_Enabled)
			.Field(&BloomComponent::m_Intensity, "Intensity", FieldType::Float, 0.01f)
			.WhenTrue(&BloomComponent::m_Enabled)
			.Field(&BloomComponent::m_Scatter, "Scatter", FieldType::Float, 0.01f)
			.WhenTrue(&BloomComponent::m_Enabled)
			.Register();
	}
}
