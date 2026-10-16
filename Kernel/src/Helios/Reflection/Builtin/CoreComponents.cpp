#include "Pch.h"
#include "Helios/Reflection/ComponentRegistration.h"
#include "Helios/Scene/Components.h"

namespace Helios
{
	/* Name / Transform / Visibility：结构性组件与显隐开关，都不进 Add 菜单；
	 * 只有 Name 不参与序列化（它写在实体的 Name 属性上）。 */
	void RegisterCoreComponents()
	{
		MakeRegistrar<NameComponent>("Name")
			.Field(&NameComponent::m_Name, "Name", FieldType::String)
			.NotSerialized()
			.NotAddable()
			.Register();

		MakeRegistrar<TransformComponent>("Transform")
			.Field(&TransformComponent::m_Position, "Position", FieldType::Vec3, 0.1f, 0.0f)
			/* 内部以弧度存储、Inspector 按角度编辑 */
			.Field(&TransformComponent::m_Rotation, "Rotation", FieldType::Vec3, 0.1f, 0.0f, "AngleDeg")
			.Field(&TransformComponent::m_Scale,    "Scale",    FieldType::Vec3, 0.1f, 1.0f, "UniformScale")
			.NotAddable()
			.Register();

		/* 可见性：层级面板行右端的眼睛开关（没有组件 = 可见）。不进「Add 菜单」（开关长在层级里）；
		 * 属性面板的 "Visible" 勾选只在实体已经隐藏时才出现。序列化元素落在 Transform 之后。 */
		MakeRegistrar<VisibilityComponent>("Visibility")
			.Field(&VisibilityComponent::m_Visible, "Visible", FieldType::Bool)
			.NotAddable()
			.Register();
	}
}
