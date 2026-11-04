#include "Pch.h"
#include "Helios/Reflection/ComponentRegistration.h"
#include "Helios/Renderer/PostProcess/Effects/Bloom.h"
#include "Helios/Renderer/PostProcess/Effects/Fog.h"
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

		/* 雾效果设置：密度与形态、雾色两节；雾色默认跟天空同源 */
		MakeRegistrar<FogComponent>("Fog")
			.Field(&FogComponent::m_Enabled, "Enabled", FieldType::Bool)
			.Group("Density")
			.Field(&FogComponent::m_Density, "Density", FieldType::Float, 0.001f)
			.Range(0.0f, 0.2f)
			.Slider()
			.Field(&FogComponent::m_HeightFalloff, "HeightFalloff", FieldType::Float, 0.001f)
			.Range(0.0f, 0.5f)
			.Slider()
			.Field(&FogComponent::m_BaseHeight, "BaseHeight", FieldType::Float, 0.5f)
			.Field(&FogComponent::m_StartDistance, "StartDistance", FieldType::Float, 1.0f)
			.Range(0.0f, 500.0f)
			.Field(&FogComponent::m_MaxOpacity, "MaxOpacity", FieldType::Float, 0.01f)
			.Range(0.0f, 1.0f)
			.Slider()
			.Field(&FogComponent::m_SkyAffect, "SkyAffect", FieldType::Float, 0.01f)
			.Range(0.0f, 1.0f)
			.Slider()
			.Group("Color")
			.Field(&FogComponent::m_UseSkyColor, "UseSkyColor", FieldType::Bool)
			.Field(&FogComponent::m_Color, "Color", FieldType::Color)
			.Field(&FogComponent::m_SunScatter, "SunScatter", FieldType::Float, 0.01f)
			.Range(0.0f, 2.0f)
			.Slider()
			.Field(&FogComponent::m_Anisotropy, "Anisotropy", FieldType::Float, 0.01f)
			.Range(-0.9f, 0.9f)
			.Slider()
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
