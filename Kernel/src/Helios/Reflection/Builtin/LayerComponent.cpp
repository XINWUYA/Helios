#include "Pch.h"
#include "Helios/Reflection/ComponentRegistration.h"
#include "Helios/Scene/Components.h"

namespace Helios
{
	/* 渲染层：对象属于哪些层。缺席的实体按默认层处理，组件自身没有副作用。 */
	void RegisterLayerComponent()
	{
		MakeRegistrar<LayerComponent>("Layer")
			.Field(&LayerComponent::m_Mask, "Layer", FieldType::LayerMask)
			.SerializeName("Mask")
			.Register();
	}
}
