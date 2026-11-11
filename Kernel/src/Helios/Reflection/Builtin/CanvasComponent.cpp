#include "Pch.h"
#include "Helios/Reflection/ComponentRegistration.h"
#include "Helios/Scene/Components.h"

namespace Helios
{
	void RegisterCanvasComponent()
	{
		/* 画布只有三项：文档、前后顺序、要不要吃输入。
		 * 文档按资源引用存（相对项目根），属性面板可以直接从资源浏览器拖一份 .rml 进来。 */
		MakeRegistrar<CanvasComponent>("Canvas")
			.Field(&CanvasComponent::m_DocumentPath, "Document", FieldType::AssetRef)
			.Field(&CanvasComponent::m_SortingOrder, "SortingOrder", FieldType::Int, 1.0f)
			.Field(&CanvasComponent::m_ReceiveInput, "ReceiveInput", FieldType::Bool)
			.Register();
	}
}
