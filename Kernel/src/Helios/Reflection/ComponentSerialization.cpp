#include "Pch.h"
#include "ComponentRegistry.h"
#include "Helios/Common/Utils.h"
#include "Helios/Scene/LayerMask.h"
#include <tinyxml2.h>

namespace Helios
{
	namespace
	{
		/* 反序列化目标地址：访问器字段先写进缓冲，随后由 CommitFieldAddress 提交 */
		void* ResolveFieldTarget(const FieldDesc& field, void* component, void* buffer)
		{
			if (field.Set != nullptr)
				return buffer;
			return static_cast<uint8_t*>(component) + field.Offset;
		}

		void CommitFieldAddress(const FieldDesc& field, void* component, void* target)
		{
			if (field.Set != nullptr)
				field.Set(component, target);
		}

		/* 属性名：默认取字段名，注册时可用 .SerializeName() 覆盖 */
		const char* SerializeAttributeName(const FieldDesc& field)
		{
			return (field.SerializeName != nullptr) ? field.SerializeName : field.Name;
		}
	}

	void SaveComponentToXml(const ComponentDesc& desc, const void* component, tinyxml2::XMLElement* element)
	{
		if (element == nullptr || component == nullptr)
			return;

		/* 自定义内容先写，使属性顺序与既有场景文件一致 */
		if (desc.SaveExtra != nullptr)
			desc.SaveExtra(element, component);

		alignas(16) uint8_t buffer[kFieldValueCapacity] = {};

		for (const FieldDesc& field : desc.Fields)
		{
			/* 条件不满足的字段不写入（如透视相机不写 HeightSize） */
			if (!EvaluateCondition(field.Condition, component))
				continue;

			const char* name = SerializeAttributeName(field);
			const void* value = ReadFieldValue(field, component, buffer);

			switch (field.Type)
			{
			case FieldType::Bool:   element->SetAttribute(name, *static_cast<const bool*>(value)); break;
			case FieldType::Float:  element->SetAttribute(name, *static_cast<const float*>(value)); break;
			case FieldType::Int:
			case FieldType::Enum:   element->SetAttribute(name, *static_cast<const int*>(value)); break;
			case FieldType::LayerMask: element->SetAttribute(name, *static_cast<const LayerMask*>(value)); break;
			case FieldType::Vec2:   element->SetAttribute(name, ToString(*static_cast<const glm::vec2*>(value)).c_str()); break;
			case FieldType::Vec3:   element->SetAttribute(name, ToString(*static_cast<const glm::vec3*>(value)).c_str()); break;
			case FieldType::Vec4:
			case FieldType::Color:  element->SetAttribute(name, ToString(*static_cast<const glm::vec4*>(value)).c_str()); break;
			case FieldType::String: element->SetAttribute(name, static_cast<const std::string*>(value)->c_str()); break;
			default: break;
			}
		}
	}

	void LoadComponentFromXml(const ComponentDesc& desc, void* component, const tinyxml2::XMLElement* element)
	{
		if (element == nullptr || component == nullptr)
			return;

		const auto load_field = [element, component](const FieldDesc& field)
		{
			/* 属性名不存在即视为该字段未保存（条件不满足时保存端不会写），无需再判条件 */
			const tinyxml2::XMLAttribute* attribute = element->FindAttribute(SerializeAttributeName(field));
			if (attribute == nullptr)
				return;

			alignas(16) uint8_t buffer[kFieldValueCapacity] = {};
			void* target = ResolveFieldTarget(field, component, buffer);

			switch (field.Type)
			{
			case FieldType::Bool:   *static_cast<bool*>(target) = attribute->BoolValue(); break;
			case FieldType::Float:  *static_cast<float*>(target) = attribute->FloatValue(); break;
			case FieldType::Int:
			case FieldType::Enum:   *static_cast<int*>(target) = attribute->IntValue(); break;
			case FieldType::LayerMask: *static_cast<LayerMask*>(target) = static_cast<LayerMask>(attribute->UnsignedValue()); break;
			case FieldType::Vec2:   *static_cast<glm::vec2*>(target) = ToVec2(attribute->Value()); break;
			case FieldType::Vec3:   *static_cast<glm::vec3*>(target) = ToVec3(attribute->Value()); break;
			case FieldType::Vec4:
			case FieldType::Color:  *static_cast<glm::vec4*>(target) = ToVec4(attribute->Value()); break;
			case FieldType::String: *static_cast<std::string*>(target) = attribute->Value(); break;
			default: return;
			}

			CommitFieldAddress(field, component, target);
		};

		for (const FieldDesc& field : desc.Fields)
			load_field(field);

		if (desc.LoadExtra != nullptr)
			desc.LoadExtra(element, component);
	}
}
