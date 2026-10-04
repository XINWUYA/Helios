#include "Pch.h"
#include "ComponentFieldCommand.h"
#include <cstring>

namespace Helios
{
	namespace
	{
		std::string MakeFieldLabel(const ComponentDesc& desc, const FieldDesc& field)
		{
			const char* component_name = (desc.Name != nullptr) ? desc.Name : "Component";
			const char* field_name = (field.Name != nullptr) ? field.Name : "Field";
			return std::string(component_name) + "." + field_name;
		}

		/* 字段值的字节数：定长字段由注册表给出（访问器字段按值的实际类型取） */
		size_t FieldByteSize(const FieldDesc& field)
		{
			return (field.ValueSize != 0) ? field.ValueSize : sizeof(float);
		}
	}

	ComponentFieldCommand::ComponentFieldCommand(Entity entity, const ComponentDesc& desc, const FieldDesc& field,
	                                             const void* before, const void* after)
		: m_Entity(entity), m_ComponentType(desc.Type), m_Field(field), m_Has(desc.Has), m_GetPtr(desc.GetPtr)
	{
		const size_t size = FieldByteSize(field);
		m_Before.assign(static_cast<const uint8_t*>(before), static_cast<const uint8_t*>(before) + size);
		m_After.assign(static_cast<const uint8_t*>(after), static_cast<const uint8_t*>(after) + size);
		m_Label = MakeFieldLabel(desc, field);
	}

	ComponentFieldCommand::ComponentFieldCommand(Entity entity, const ComponentDesc& desc, const FieldDesc& field,
	                                             const std::string& before, const std::string& after)
		: m_Entity(entity), m_ComponentType(desc.Type), m_Field(field), m_Has(desc.Has), m_GetPtr(desc.GetPtr),
		  m_BeforeText(before), m_AfterText(after), m_IsText(true)
	{
		m_Label = MakeFieldLabel(desc, field);
	}

	bool ComponentFieldCommand::IsSameField(const ComponentFieldCommand& other) const
	{
		/* 偏移定位的字段用偏移区分，访问器字段用 Get 回调区分（其偏移恒为 0） */
		return other.m_ComponentType == m_ComponentType
			&& other.m_Field.Type == m_Field.Type
			&& other.m_Field.Offset == m_Field.Offset
			&& other.m_Field.Get == m_Field.Get;
	}

	void ComponentFieldCommand::Write(const void* bytes, const std::string* text)
	{
		if (!m_Entity || m_Has == nullptr || m_GetPtr == nullptr)
			return;

		/* 组件已被移除、或实体已销毁时静默跳过 */
		if (!m_Has(m_Entity))
			return;

		void* component = m_GetPtr(m_Entity);
		if (component == nullptr)
			return;

		/* 访问器字段：交给 setter */
		if (m_Field.Set != nullptr)
		{
			if (bytes != nullptr)
				m_Field.Set(component, bytes);
			return;
		}

		uint8_t* target = static_cast<uint8_t*>(component) + m_Field.Offset;

		if (text != nullptr)
		{
			*reinterpret_cast<std::string*>(target) = *text;
			return;
		}

		if (bytes != nullptr)
			std::memcpy(target, bytes, FieldByteSize(m_Field));
	}

	void ComponentFieldCommand::Do()
	{
		Write(m_IsText ? nullptr : m_After.data(), m_IsText ? &m_AfterText : nullptr);
	}

	void ComponentFieldCommand::Undo()
	{
		Write(m_IsText ? nullptr : m_Before.data(), m_IsText ? &m_BeforeText : nullptr);
	}

	bool ComponentFieldCommand::TryMerge(const ICommand& next)
	{
		const auto* other = dynamic_cast<const ComponentFieldCommand*>(&next);
		if (other == nullptr || !(other->m_Entity == m_Entity) || !IsSameField(*other))
			return false;

		/* 保留最初的起点，吸收最新的落点 */
		m_After = other->m_After;
		m_AfterText = other->m_AfterText;
		return true;
	}
}
