#include "Pch.h"
#include "Helios/Reflection/ComponentRegistration.h"
#include "Helios/Renderer/PostProcess/Effects/Bloom.h"
#include "Helios/Renderer/PostProcess/Effects/Taa.h"
#include "Helios/Scene/Components.h"

namespace Helios
{
	void RegisterPostProcessComponents()
	{
		/* 后处理对象：总开关（关闭 = 整个对象不参与解析，效果设置保留）；效果设置组件挂在同一实体上 */
		MakeRegistrar<PostProcessComponent>("PostProcess")
			.Field(&PostProcessComponent::m_Enabled, "Enabled", FieldType::Bool)
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

		/* TAA 效果设置：历史占比与锐化同样只在启用时写出 */
		MakeRegistrar<TaaComponent>("TAA")
			.Field(&TaaComponent::m_Enabled, "Enabled", FieldType::Bool)
			.Field(&TaaComponent::m_HistoryWeight, "HistoryWeight", FieldType::Float, 0.01f)
			.WhenTrue(&TaaComponent::m_Enabled)
			.Field(&TaaComponent::m_Sharpness, "Sharpness", FieldType::Float, 0.01f)
			.WhenTrue(&TaaComponent::m_Enabled)
			.Register();
	}
}
