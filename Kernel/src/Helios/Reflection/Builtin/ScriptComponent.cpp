#include "Pch.h"
#include "Helios/Reflection/ComponentRegistration.h"
#include "Helios/Scene/Components.h"
#include <cstdlib>
#include <tinyxml2.h>

namespace Helios
{
	namespace
	{
		void SaveScriptComponent(tinyxml2::XMLElement* element, const void* component)
		{
			const auto& script_component = *static_cast<const ScriptComponent*>(component);
			for (const ScriptSlot& slot : script_component.m_Scripts)
			{
				tinyxml2::XMLElement* entry = element->InsertNewChildElement("Entry");
				entry->SetAttribute("Class", slot.ClassName.c_str());
				entry->SetAttribute("Order", slot.m_Order);
				for (const ScriptProperty& property : slot.Properties)
					entry->SetAttribute(property.Name.c_str(), property.Value.c_str());
			}
		}

		void LoadScriptComponent(const tinyxml2::XMLElement* element, void* component)
		{
			auto& script_component = *static_cast<ScriptComponent*>(component);
			script_component.m_Scripts.clear();

			/* 先按文本收：此时拿不到字段表也能把配置保住，等类可用时再对类型 */
			const auto load_one = [&script_component](const tinyxml2::XMLElement* source)
			{
				ScriptSlot slot;
				const char* class_name = source->Attribute("Class");
				slot.ClassName = (class_name != nullptr) ? class_name : "";

				for (const tinyxml2::XMLAttribute* attribute = source->FirstAttribute();
					attribute != nullptr; attribute = attribute->Next())
				{
					const std::string name = attribute->Name();
					if (name == "Class")
						continue;
					if (name == "Order")
					{
						slot.m_Order = std::atoi(attribute->Value());
						continue;
					}

					slot.Properties.emplace_back(name, FieldType::String, attribute->Value());
				}

				script_component.m_Scripts.emplace_back(std::move(slot));
			};

			/* 新格式 <Script><Entry .../></Script>；旧场景是属性直接挂在 <Script> 上 */
			bool has_entry = false;
			for (const tinyxml2::XMLElement* entry = element->FirstChildElement("Entry");
				entry != nullptr; entry = entry->NextSiblingElement("Entry"))
			{
				has_entry = true;
				load_one(entry);
			}

			if (!has_entry && element->Attribute("Class") != nullptr)
				load_one(element);
		}
	}

	void RegisterScriptComponent()
	{
		/* 字段表留空：脚本参数随类变化，全部由编辑器侧的自定义绘制补上，
		 * SaveExtra 只按文本存值。 */
		MakeRegistrar<ScriptComponent>("Script")
			.SaveExtra(&SaveScriptComponent)
			.LoadExtra(&LoadScriptComponent)
			.Register();
	}
}
