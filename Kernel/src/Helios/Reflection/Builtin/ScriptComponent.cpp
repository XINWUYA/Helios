#include "Pch.h"
#include "Helios/ImGui/ImGuiExtensions.h"
#include "Helios/Reflection/ComponentRegistration.h"
#include "Helios/Scene/Components.h"
#include "Helios/Scene/Scene.h"
#include "Helios/Script/ScriptRegistry.h"
#include <algorithm>
#include <imgui.h>
#include <tinyxml2.h>

namespace Helios
{
	namespace
	{
		/* 参数表与当前类的字段表对不上（刚换类、脚本库重载过）就按字段表重建：
		 * 同名同类型的值留着，新字段取默认值，字段没了的直接丢。 */
		bool PropertiesInSync(const ScriptSlot& slot, const ScriptClassDesc& desc)
		{
			if (slot.Properties.size() != desc.Fields.size())
				return false;

			for (const ScriptFieldDesc& field : desc.Fields)
			{
				const auto it = std::find_if(slot.Properties.begin(), slot.Properties.end(),
					[&field](const ScriptProperty& property) { return property.Name == field.Name; });
				if (it == slot.Properties.end() || it->Type != field.Type)
					return false;
			}
			return true;
		}

		void RebuildProperties(ScriptSlot& slot, const ScriptClassDesc& desc)
		{
			std::vector<ScriptProperty> rebuilt;

			/* 默认值从一个临时实例读：字段初值写在类里，这里不重复维护一份 */
			Script* probe = (desc.Create != nullptr) ? desc.Create() : nullptr;
			for (const ScriptFieldDesc& field : desc.Fields)
			{
				ScriptProperty property;
				property.Name = field.Name;
				property.Type = field.Type;
				if (field.GetEnumNames != nullptr)
					property.Options = field.GetEnumNames();

				const auto it = std::find_if(slot.Properties.begin(), slot.Properties.end(),
					[&field](const ScriptProperty& existing)
					{
						return existing.Name == field.Name && existing.Type == field.Type;
					});

				if (it != slot.Properties.end())
					property.Value = it->Value;
				else if (probe != nullptr)
					property.Value = ReadScriptPropertyText(*probe, field);

				rebuilt.emplace_back(std::move(property));
			}

			if (probe != nullptr && desc.Destroy != nullptr)
				desc.Destroy(probe);

			slot.Properties = std::move(rebuilt);
		}

		/* 拖进来的实体句柄 → 名字（面板只存名字，脚本用 FindEntity 解析回去） */
		std::string ResolveEntityName(const Scene* scene, entt::entity handle)
		{
			if (scene == nullptr)
				return {};

			const entt::registry& registry = scene->GetRegistry();
			if (!registry.valid(handle))
				return {};

			const auto* name = registry.try_get<NameComponent>(handle);
			return (name != nullptr) ? name->m_Name : std::string();
		}

		/* 画一行参数：按类型走通用控件，返回值是否改动 */
		bool DrawPropertyValue(ScriptProperty& property, bool read_only, const Scene* scene)
		{
			bool changed = false;
			if (read_only)
				ImGui::BeginDisabled();

			switch (property.Type)
			{
			case FieldType::Bool:
			{
				bool value = (property.Value == "1" || property.Value == "true");
				if (ImGuiExt::DrawCheckboxUI(property.Name, value))
				{
					property.Value = value ? "1" : "0";
					changed = true;
				}
				break;
			}
			case FieldType::Int:
			{
				int value = 0;
				try { value = std::stoi(property.Value); } catch (const std::exception&) {}
				ImGuiExt::DrawDragIntUI(property.Name.c_str(), value, 1.0f);
				const std::string text = std::to_string(value);
				if (text != property.Value)
				{
					property.Value = text;
					changed = true;
				}
				break;
			}
			case FieldType::Enum:
			{
				int value = 0;
				try { value = std::stoi(property.Value); } catch (const std::exception&) {}

				/* 类缺失时没有名字表，退回按数字编辑 */
				if (property.Options.empty())
				{
					ImGuiExt::DrawDragIntUI(property.Name.c_str(), value, 1.0f);
				}
				else
				{
					value = std::clamp(value, 0, static_cast<int>(property.Options.size()) - 1);
					const int before = value;
					ImGuiExt::DrawComboUI(property.Name, property.Options, value);
					if (value != before)
					{
						property.Value = std::to_string(value);
						changed = true;
					}
					break;
				}

				const std::string text = std::to_string(value);
				if (text != property.Value)
				{
					property.Value = text;
					changed = true;
				}
				break;
			}
			case FieldType::Float:
			{
				float value = 0.0f;
				try { value = std::stof(property.Value); } catch (const std::exception&) {}
				ImGuiExt::DrawDragFloatUI(property.Name, value, 0.1f);
				const std::string text = ToString(value);
				if (text != property.Value)
				{
					property.Value = text;
					changed = true;
				}
				break;
			}
			case FieldType::Vec2:
			{
				glm::vec2 value = ToVec2(property.Value);
				ImGuiExt::DrawDragFloat2UI(property.Name.c_str(), value);
				const std::string text = ToString(value);
				if (text != property.Value)
				{
					property.Value = text;
					changed = true;
				}
				break;
			}
			case FieldType::Vec3:
			{
				glm::vec3 value = ToVec3(property.Value);
				ImGuiExt::DrawDragFloat3UI(property.Name.c_str(), value);
				const std::string text = ToString(value);
				if (text != property.Value)
				{
					property.Value = text;
					changed = true;
				}
				break;
			}
			case FieldType::Vec4:
			{
				glm::vec4 value = ToVec4(property.Value);
				ImGuiExt::DrawDragFloat4UI(property.Name.c_str(), value);
				const std::string text = ToString(value);
				if (text != property.Value)
				{
					property.Value = text;
					changed = true;
				}
				break;
			}
			case FieldType::Color:
			{
				glm::vec4 value = ToVec4(property.Value);
				ImGuiExt::DrawColorUI(property.Name, value);
				const std::string text = ToString(value);
				if (text != property.Value)
				{
					property.Value = text;
					changed = true;
				}
				break;
			}
			case FieldType::AssetRef:
			{
				char buffer[512] = {};
				std::snprintf(buffer, sizeof(buffer), "%s", property.Value.c_str());
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::InputText(property.Name.c_str(), buffer, sizeof(buffer)))
				{
					property.Value = buffer;
					changed = true;
				}

				/* 从资源浏览器拖文件过来：payload 就是路径字符串 */
				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RESOURCE_BROWSER_ITEM"))
					{
						property.Value = static_cast<const char*>(payload->Data);
						changed = true;
					}
					ImGui::EndDragDropTarget();
				}

				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Drop an asset from the Resource Browser");
				break;
			}
			case FieldType::EntityRef:
			{
				char buffer[256] = {};
				std::snprintf(buffer, sizeof(buffer), "%s", property.Value.c_str());
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::InputText(property.Name.c_str(), buffer, sizeof(buffer)))
				{
					property.Value = buffer;
					changed = true;
				}

				/* 从层级面板拖节点过来：payload 是实体句柄，按名字存 */
				if (ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_HIERARCHY_ENTITY"))
					{
						const auto handle = *static_cast<const entt::entity*>(payload->Data);
						const std::string name = ResolveEntityName(scene, handle);
						if (!name.empty())
						{
							property.Value = name;
							changed = true;
						}
					}
					ImGui::EndDragDropTarget();
				}

				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Drop an entity from the Scene Hierarchy");
				break;
			}
			default:
			{
				/* 字符串与认不出的类型：按文本编辑，脚本自己解析 */
				char buffer[256] = {};
				std::snprintf(buffer, sizeof(buffer), "%s", property.Value.c_str());
				if (ImGui::InputText(property.Name.c_str(), buffer, sizeof(buffer)))
				{
					property.Value = buffer;
					changed = true;
				}
				break;
			}
			}

			if (read_only)
				ImGui::EndDisabled();

			return changed;
		}

		/* 一个挂载点：类下拉 + 移除 + 参数 */
		bool DrawScriptSlot(ScriptSlot& slot, int* remove_index, int index, const Scene* scene)
		{
			bool changed = false;

			const auto& classes = ScriptRegistry::Instance().All();
			std::vector<std::string> options;
			options.emplace_back("(None)");
			int selected = 0;
			for (size_t i = 0; i < classes.size(); ++i)
			{
				options.emplace_back(classes[i].Name != nullptr ? classes[i].Name : "");
				if (slot.ClassName == options.back())
					selected = static_cast<int>(i) + 1;
			}

			/* Runtime 下实例在手，换类会跟当前实例对不上，锁住 */
			const bool read_only = (slot.Instance != nullptr);
			if (read_only)
				ImGui::BeginDisabled();

			const ImGuiStyle& style = ImGui::GetStyle();
			const float remove_width = 62.0f;
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - remove_width - style.ItemSpacing.x);
			if (ImGui::BeginCombo("##Class", options[static_cast<size_t>(selected)].c_str()))
			{
				for (int i = 0; i < static_cast<int>(options.size()); ++i)
				{
					if (ImGui::Selectable(options[static_cast<size_t>(i)].c_str(), i == selected))
					{
						slot.ClassName = (i <= 0) ? std::string() : options[static_cast<size_t>(i)];
						slot.Properties.clear();
						changed = true;
					}
				}
				ImGui::EndCombo();
			}

			ImGui::SameLine();
			if (ImGui::Button("Remove", ImVec2(remove_width, 0)))
				*remove_index = index;

			if (read_only)
				ImGui::EndDisabled();

			if (slot.ClassName.empty())
			{
				ImGui::TextDisabled("Pick a script class.");
				return changed;
			}

			const ScriptClassDesc* desc = ScriptRegistry::Instance().Find(slot.ClassName);
			if (desc == nullptr)
			{
				/* 类缺失：参数按文本原样留着，换台机器打开也不丢配置 */
				ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.30f, 1.0f),
					"Script class '%s' not found. Parameters are kept as text.", slot.ClassName.c_str());
				for (ScriptProperty& property : slot.Properties)
				{
					char buffer[256] = {};
					std::snprintf(buffer, sizeof(buffer), "%s", property.Value.c_str());
					if (ImGui::InputText(property.Name.c_str(), buffer, sizeof(buffer)))
					{
						property.Value = buffer;
						changed = true;
					}
				}
				return changed;
			}

			if (!PropertiesInSync(slot, *desc))
				RebuildProperties(slot, *desc);

			for (ScriptProperty& property : slot.Properties)
			{
				if (DrawPropertyValue(property, read_only, scene))
					changed = true;
			}

			if (slot.Properties.empty())
				ImGui::TextDisabled("No editable fields.");

			return changed;
		}

		bool DrawScriptComponent(void* component)
		{
			auto& script_component = *static_cast<ScriptComponent*>(component);

			bool changed = false;
			int remove_index = -1;

			for (size_t i = 0; i < script_component.m_Scripts.size(); ++i)
			{
				if (i > 0)
					ImGui::Separator();

				ImGui::PushID(static_cast<int>(i));
				if (DrawScriptSlot(script_component.m_Scripts[i], &remove_index,
					static_cast<int>(i), script_component.m_pOwnerScene))
					changed = true;
				ImGui::PopID();
			}

			if (script_component.m_Scripts.empty())
				ImGui::TextDisabled("No script attached.");

			/* 运行中不给加：新槽位没有实例，后续槽位下标也会跟运行时的记录对不上 */
			const bool running = std::any_of(script_component.m_Scripts.begin(), script_component.m_Scripts.end(),
				[](const ScriptSlot& slot) { return slot.Instance != nullptr; });

			const float button_width = ImGui::GetContentRegionAvail().x;
			ImGui::BeginDisabled(running);
			if (ImGui::Button("Add Script", ImVec2(button_width, 0.0f)))
				ImGui::OpenPopup("##AddScript");
			ImGui::EndDisabled();

			if (ImGui::BeginPopup("##AddScript"))
			{
				const auto& classes = ScriptRegistry::Instance().All();
				if (classes.empty())
					ImGui::TextDisabled("No script classes loaded.");

				for (const ScriptClassDesc& desc : classes)
				{
					if (desc.Name != nullptr && ImGui::MenuItem(desc.Name))
					{
						ScriptSlot slot;
						slot.ClassName = desc.Name;
						script_component.m_Scripts.emplace_back(std::move(slot));
						changed = true;
					}
				}
				ImGui::EndPopup();
			}

			if (remove_index >= 0 && remove_index < static_cast<int>(script_component.m_Scripts.size()))
			{
				script_component.m_Scripts.erase(script_component.m_Scripts.begin() + remove_index);
				changed = true;
			}

			return changed;
		}

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
		/* 字段表留空：脚本参数随类变化，全部由 CustomDraw 画、由 SaveExtra 写。
		 * 换类与脚本库重载后的参数对齐也在 CustomDraw 里做。 */
		MakeRegistrar<ScriptComponent>("Script")
			.CustomDraw(&DrawScriptComponent)
			.SaveExtra(&SaveScriptComponent)
			.LoadExtra(&LoadScriptComponent)
			.Register();
	}
}
