#include "Pch.h"
#include "ComponentRegistry.h"
#include "Helios/Application/AssetManager.h"
#include "Helios/Scene/Components.h"
#include "Helios/Scene/Entity.h"
#include "Helios/Scene/SceneCommon.h"
#include "Helios/Common/Utils.h"
#include "Helios/ImGui/EditorTheme.h"
#include "Helios/ImGui/ImGuiExtensions.h"
#include "Helios/VirtualDevice/DeviceTexture.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <imgui.h>
#include <tinyxml2.h>
#include <type_traits>

namespace Helios
{
	ComponentRegistry& ComponentRegistry::Instance()
	{
		/* 函数内静态：首次使用时构造，规避跨 TU 的静态初始化顺序问题
		 * （内部注册对象会在静态初始化期调用本函数）。 */
		static ComponentRegistry s_Instance;
		return s_Instance;
	}

	const ComponentDesc* ComponentRegistry::Find(std::type_index type) const
	{
		for (const auto& component : m_Components)
		{
			if (component.Type == type)
				return &component;
		}
		return nullptr;
	}

	ComponentRegistrar::ComponentRegistrar(const char* name, std::type_index type)
	{
		m_Desc.Name = name;
		m_Desc.Type = type;
	}

	ComponentRegistrar& ComponentRegistrar::Field(FieldAccessor accessor, const char* name, FieldType type,
	                                              float drag_speed, float reset_value, const char* semantics)
	{
		FieldDesc desc;
		desc.Name = name;
		desc.Type = type;
		desc.ValueSize = accessor.ValueSize;
		desc.DragSpeed = drag_speed;
		desc.ResetValue = reset_value;
		desc.Semantics = semantics;
		desc.Get = accessor.Get;
		desc.Set = accessor.Set;
		m_Desc.Fields.emplace_back(desc);
		return *this;
	}

	ComponentRegistrar& ComponentRegistrar::EnumNames(EnumNamesFunc names)
	{
		if (!m_Desc.Fields.empty())
			m_Desc.Fields.back().GetEnumNames = names;
		return *this;
	}

	ComponentRegistrar& ComponentRegistrar::Accessors(HasFunc has, GetPtrFunc get, RemoveFunc remove, AddFunc add)
	{
		m_Desc.Has = has;
		m_Desc.GetPtr = get;
		m_Desc.Remove = remove;
		m_Desc.Add = add;
		return *this;
	}

	ComponentRegistrar& ComponentRegistrar::CustomDraw(ComponentDrawFunc draw)
	{
		m_Desc.CustomDraw = draw;
		return *this;
	}

	ComponentRegistrar& ComponentRegistrar::Visible(ComponentVisibleFunc visible)
	{
		m_Desc.Visible = visible;
		return *this;
	}

	ComponentRegistrar& ComponentRegistrar::Snapshot(ComponentCaptureFunc capture, ComponentRestoreFunc restore)
	{
		m_Desc.Capture = capture;
		m_Desc.Restore = restore;
		return *this;
	}

	ComponentRegistrar& ComponentRegistrar::NotAddable()
	{
		m_Desc.bAddable = false;
		return *this;
	}

	ComponentRegistrar& ComponentRegistrar::Variant(const char* menu_name, AddFunc add)
	{
		m_Desc.Variants.push_back({ menu_name, add });
		return *this;
	}

	ComponentRegistrar& ComponentRegistrar::SerializeName(const char* name)
	{
		if (!m_Desc.Fields.empty())
			m_Desc.Fields.back().SerializeName = name;
		return *this;
	}

	ComponentRegistrar& ComponentRegistrar::SaveExtra(ComponentSaveFunc save)
	{
		m_Desc.SaveExtra = save;
		return *this;
	}

	ComponentRegistrar& ComponentRegistrar::LoadExtra(ComponentLoadFunc load)
	{
		m_Desc.LoadExtra = load;
		return *this;
	}

	ComponentRegistrar& ComponentRegistrar::NotSerialized()
	{
		m_Desc.bSerializable = false;
		return *this;
	}

	void ComponentRegistrar::Register()
	{
		/* 未显式声明形态时，以默认构造作为唯一形态（菜单用组件名） */
		if (m_Desc.bAddable && m_Desc.Variants.empty() && m_Desc.Add != nullptr)
			m_Desc.Variants.push_back({ nullptr, m_Desc.Add });

		ComponentRegistry::Instance().Add(std::move(m_Desc));
	}

	ComponentRegistrar& ComponentRegistrar::When(FieldCondition condition)
	{
		if (!m_Desc.Fields.empty())
			m_Desc.Fields.back().Condition = condition;
		return *this;
	}

	namespace
	{
		/* 按依赖字段的类型读取其数值（供比较类条件使用） */
		double ReadDependNumber(FieldType type, const void* field)
		{
			switch (type)
			{
			case FieldType::Bool:  return *static_cast<const bool*>(field) ? 1.0 : 0.0;
			case FieldType::Int:   return static_cast<double>(*static_cast<const int*>(field));
			case FieldType::Float: return static_cast<double>(*static_cast<const float*>(field));
			default:               return 0.0;
			}
		}
	}

	bool EvaluateCondition(const FieldCondition& condition, const void* component)
	{
		if (component == nullptr)
			return true;

		if (condition.Predicate != nullptr)
			return condition.Predicate(component);

		/* 无依赖字段：不读组件内存 */
		if (condition.Op == ConditionOp::Always)
			return true;

		const void* field = static_cast<const uint8_t*>(component) + condition.DependOffset;

		switch (condition.Op)
		{
		case ConditionOp::IsTrue:   return *static_cast<const bool*>(field);
		case ConditionOp::IsFalse:  return !*static_cast<const bool*>(field);
		case ConditionOp::Equal:    return ReadDependNumber(condition.DependType, field) == condition.Expected;
		case ConditionOp::NotEqual: return ReadDependNumber(condition.DependType, field) != condition.Expected;
		case ConditionOp::Greater:  return ReadDependNumber(condition.DependType, field) > condition.Expected;
		case ConditionOp::Less:     return ReadDependNumber(condition.DependType, field) < condition.Expected;
		case ConditionOp::Always:   break;
		}
		return true;
	}

	const void* ReadFieldValue(const FieldDesc& field, const void* component, void* buffer)
	{
		if (component == nullptr)
			return nullptr;

		if (field.Get != nullptr)
		{
			field.Get(component, buffer);
			return buffer;
		}

		return static_cast<const uint8_t*>(component) + field.Offset;
	}

	namespace
	{
		/* 字段值所在地址：偏移字段直接取址，访问器字段拷进缓冲 */
		const void* ResolveFieldAddress(const FieldDesc& field, const void* component, void* buffer)
		{
			return ReadFieldValue(field, component, buffer);
		}

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
			const void* value = ResolveFieldAddress(field, component, buffer);

			switch (field.Type)
			{
			case FieldType::Bool:   element->SetAttribute(name, *static_cast<const bool*>(value)); break;
			case FieldType::Float:  element->SetAttribute(name, *static_cast<const float*>(value)); break;
			case FieldType::Int:
			case FieldType::Enum:   element->SetAttribute(name, *static_cast<const int*>(value)); break;
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

	namespace
	{
		/* ---- 自定义绘制：含嵌套对象或资源引用的组件 ----
		 * 这些字段靠访问器读写（如 camera->GetFov()），无法用「偏移 + 类型」表达，整块交给回调。 */

		/* 贴图与模型是资源引用：字段表只认 POD 与字符串，路径读写单独处理；
		 * 对象为空时不写属性，读的时候也接受属性缺失。 */
		void SaveSpriteTexture(tinyxml2::XMLElement* element, const void* component)
		{
			const auto& texture = static_cast<const SpriteComponent*>(component)->m_Texture;
			if (texture != nullptr)
				element->SetAttribute("TexturePath", RELATIVE_PATH(texture->GetPath()).c_str());
		}

		void LoadSpriteTexture(const tinyxml2::XMLElement* element, void* component)
		{
			const char* path = element->Attribute("TexturePath");
			if (path != nullptr && *path != '\0')
				/* 属性里存的是相对资产根的形态，入口即转绝对（对象内部路径恒为绝对） */
				static_cast<SpriteComponent*>(component)->m_Texture = DeviceTexture::Create(ABSOLUTE_PATH(path));
		}

		void SaveModelPath(tinyxml2::XMLElement* element, const void* component)
		{
			const auto& model_component = *static_cast<const ModelComponent*>(component);
			const auto& model = model_component.m_Model;
			if (model != nullptr)
			{
				/* 内建模型（BuiltinCube 等）没有资产文件：存身份名，加载经
				 * Model::Create 反查回 BuiltinModelType；文件模型存相对路径。 */
				BuiltinModelType builtin_type{};
				const bool is_builtin = model->TryGetBuiltinType(builtin_type);
				element->SetAttribute("ModelPath", is_builtin
					? model->GetPath().c_str()
					: RELATIVE_PATH(model->GetPath()).c_str());
			}

			/* 槽的实体级绑定：引用形态只写路径；实例形态全量内嵌（不依赖来源资产存活） */
			for (const auto& entry : model_component.m_SlotOverrides)
			{
				const auto& slot_override = entry.second;
				if (slot_override.pMaterial == nullptr)
					continue;

				auto* override_doc = element->InsertNewChildElement("MaterialOverride");
				override_doc->SetAttribute("Slot", entry.first.c_str());

				if (slot_override.IsInstance)
				{
					override_doc->SetAttribute("Instance", true);
					if (!slot_override.SourceAssetPath.empty())
						override_doc->SetAttribute("Source", RELATIVE_PATH(slot_override.SourceAssetPath).c_str());
					MaterialIO::WriteBody(override_doc, *slot_override.pMaterial);
				}
				else
				{
					/* "覆盖"约定挂的是材质资产；运行时构造的匿名材质没有路径可存，跳过 */
					const auto& material_path = slot_override.pMaterial->GetPath();
					if (material_path.empty())
						continue;
					override_doc->SetAttribute("Asset", RELATIVE_PATH(material_path).c_str());
				}
			}
		}

		void LoadModelPath(const tinyxml2::XMLElement* element, void* component)
		{
			auto& model_component = *static_cast<ModelComponent*>(component);

			const char* path = element->Attribute("ModelPath");
			if (path != nullptr && *path != '\0')
				model_component.m_Model = Model::Create(path);

			/* 槽绑定：引用形态走 MaterialAssetManager；实例形态读内嵌定义 */
			model_component.m_SlotOverrides.clear();
			for (const tinyxml2::XMLElement* override_doc = element->FirstChildElement("MaterialOverride"); override_doc; override_doc = override_doc->NextSiblingElement("MaterialOverride"))
			{
				const char* slot_name = override_doc->Attribute("Slot");
				if (slot_name == nullptr || *slot_name == '\0')
					continue;

				MaterialSlotOverride slot_override;
				if (override_doc->BoolAttribute("Instance"))
				{
					auto instance = CreateSharedPtr<Material>();
					if (!MaterialIO::ReadBody(override_doc, *instance))
						continue;
					slot_override.pMaterial = std::move(instance);
					slot_override.IsInstance = true;
					if (const char* source = override_doc->Attribute("Source"))
						slot_override.SourceAssetPath = ABSOLUTE_PATH(source);
				}
				else
				{
					const char* asset = override_doc->Attribute("Asset");
					if (asset == nullptr || *asset == '\0')
						continue;
					auto material = MaterialAssetManager::Instance().GetOrLoad(ABSOLUTE_PATH(asset));
					if (material == nullptr)
					{
						CORE_LOG_WARN("Failed to load override material '{}' for slot '{}'.", asset, slot_name);
						continue;
					}
					slot_override.pMaterial = std::move(material);
				}

				model_component.m_SlotOverrides[slot_name] = std::move(slot_override);
			}
		}

		/* 贴图是资源引用，需要拖入与悬停预览，故整块自定义绘制；
		 * BaseColor 与 TilingFactor 走 schema 字段（见注册处）。
		 * 返回是否改动：换贴图要生成快照命令（撤销与场景脏标记都靠它）。 */
		bool DrawSpriteBlock(void* raw)
		{
			auto& component = *static_cast<SpriteComponent*>(raw);
			const SharedPtr<DeviceTexture> before = component.m_Texture;
			ImGuiExt::DrawTextureUI("Texture", component.m_Texture);
			return component.m_Texture != before;
		}

		/* 投影类型决定哪些参数有意义：透视看 Fov，正交看 HeightSize */
		bool IsPerspectiveProjection(const void* component)
		{
			const auto& camera = static_cast<const CameraComponent*>(component)->m_Camera;
			return camera != nullptr && camera->GetProjectionType() == CameraProjectionType::Perspective;
		}

		bool IsOrthographicProjection(const void* component)
		{
			const auto& camera = static_cast<const CameraComponent*>(component)->m_Camera;
			return camera != nullptr && camera->GetProjectionType() == CameraProjectionType::Orthographic;
		}

		/* ==================== 材质槽区块 ====================
		 * 每槽一行（槽名 | 材质名输入框 + 挂载 / 清除 / 重置）= Model 卡里保留的"材质引用"。
		 * 材质详情（Shader 和参数）在 SceneHierarchy 的"材质卡"里（通道不能嵌套、只能同级平铺）。 */

		/* 资源引用输入框：外观是标准输入框、只读展示资源名（文本可选中复制），
		 * 拖放目标与 tooltip 由调用方挂在控件上；空文本时显示 hint（弱化色）。 */
		void DrawRefInputBox(const char* id, const std::string& text, float width, const char* hint)
		{
			char buffer[256] = {};
			std::strncpy(buffer, text.c_str(), sizeof(buffer) - 1);

			ImGui::SetNextItemWidth(width);
			ImGui::InputTextWithHint(id, hint, buffer, sizeof(buffer), ImGuiInputTextFlags_ReadOnly);
		}

		/* 槽行：槽名 | 材质名输入框（只读展示 + 拖拽挂载 .mtl）+ 清除 / 重置。
		 * 输入框显示材质名，实例额外带 [实例] 标记（参数独立于来源资产，要分得开）。 */
		bool DrawMaterialSlotRow(ModelComponent& component, const Model& model, int slot_index)
		{
			const MaterialSlot* slot = model.GetSlotByIndex(slot_index);
			if (slot == nullptr)
				return false;

			bool changed = false;

			const auto iter = component.m_SlotOverrides.find(slot->Name);
			const bool has_override = (iter != component.m_SlotOverrides.end() && iter->second.pMaterial != nullptr);
			const bool is_instance = has_override && iter->second.IsInstance;

			/* 值显示：材质名 + 实例标记；完整路径进 tooltip */
			std::string display;
			std::string tooltip_path;
			if (has_override)
			{
				if (is_instance)
				{
					const bool has_source = !iter->second.SourceAssetPath.empty();
					display = (has_source
						? PathToUtf8(PathFromUtf8(iter->second.SourceAssetPath).filename())
						: std::string("(material instance)")) + "   [实例]";
					tooltip_path = has_source
						? RELATIVE_PATH(iter->second.SourceAssetPath)
						: std::string("(material instance)");
				}
				else
				{
					/* "覆盖"约定挂的是材质资产；运行时构造的匿名材质没有路径 */
					const std::string& material_path = iter->second.pMaterial->GetPath();
					display = material_path.empty()
						? std::string("(material)")
						: PathToUtf8(PathFromUtf8(material_path).filename());
					tooltip_path = material_path.empty() ? std::string("(material)") : RELATIVE_PATH(material_path);
				}
			}
			else if (slot->pDefault != nullptr && !slot->pDefault->GetPath().empty())
			{
				display = PathToUtf8(PathFromUtf8(slot->pDefault->GetPath()).filename());
				tooltip_path = RELATIVE_PATH(slot->pDefault->GetPath());
			}
			else
			{
				display = "Builtin White";
				tooltip_path = "(builtin white)";
			}

			const float gap = ImGui::GetStyle().ItemInnerSpacing.x;

			const float value_width = ImGuiExt::BeginPropertyRow(slot->Name.c_str());

			/* 行内自算尺寸（X / Reset 按钮高）必须在 Begin 之后取：行高由
			 * BeginPropertyRow 统一压成"字体高"，这里要跟行内实际控件一致 */
			const float frame_height = ImGui::GetFrameHeight();

			/* 右端按钮区：清除（X）+ 实例时的重置 */
			const float reset_width = is_instance
				? ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2.0f
				: 0.0f;
			float button_zone = has_override ? (frame_height + gap) : 0.0f;
			if (is_instance)
				button_zone += reset_width + gap;

			const float target_width = std::max(1.0f, value_width - button_zone);

			DrawRefInputBox("##MaterialRef", display, target_width, nullptr);

			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s\n\nDrag a .mtl material asset here to assign or replace this slot's material",
					tooltip_path.c_str());

			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RESOURCE_BROWSER_ITEM"))
				{
					std::filesystem::path relative_path;
					if (payload->DataSize > 1 && TryPathFromUtf8Payload(
						payload->Data, static_cast<size_t>(payload->DataSize), relative_path))
					{
						std::string extension = PathToUtf8(relative_path.extension());
						std::transform(extension.begin(), extension.end(), extension.begin(),
							[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

						if (extension == ".mtl")
						{
							const std::string absolute_path = PathToUtf8(g_AssetsPath / relative_path);
							auto material = MaterialAssetManager::Instance().GetOrLoad(absolute_path);
							if (material != nullptr)
							{
								MaterialSlotOverride slot_override;
								slot_override.pMaterial = std::move(material);
								component.m_SlotOverrides[slot->Name] = std::move(slot_override);
								changed = true;
							}
						}
					}
				}
				ImGui::EndDragDropTarget();
			}

			if (is_instance)
			{
				ImGui::SameLine(0.0f, gap);
				if (ImGui::Button("Reset", ImVec2(reset_width, frame_height)))
				{
					/* 从来源资产重新克隆（无来源 = 回默认） */
					if (!iter->second.SourceAssetPath.empty())
					{
						auto source = MaterialAssetManager::Instance().GetOrLoad(iter->second.SourceAssetPath);
						if (source != nullptr)
						{
							auto& slot_override = component.m_SlotOverrides[slot->Name];
							slot_override.pMaterial = source->Clone();
							slot_override.IsInstance = true;
							changed = true;
						}
					}
					else
					{
						component.m_SlotOverrides.erase(slot->Name);
						changed = true;
					}
				}

				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Re-clone from the source asset (or fall back to default)");
			}

			if (has_override)
			{
				ImGui::SameLine(0.0f, gap);
				if (ImGui::Button("X", ImVec2(frame_height, frame_height)))
				{
					component.m_SlotOverrides.erase(slot->Name);
					changed = true;
				}

				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Clear override / instance, fall back to the model default");
			}

			ImGuiExt::EndPropertyRow();
			return changed;
		}

		/* 材质槽区块入口：标题 + 统计 + 每槽一行引用（详情在 SceneHierarchy 的材质卡里） */
		bool DrawMaterialSlots(ModelComponent& component, const Model& model)
		{
			const auto& slots = model.GetMaterialSlots();
			if (slots.empty())
				return false;

			bool changed = false;

			int overridden_count = 0;
			for (const auto& slot : slots)
			{
				const auto iter = component.m_SlotOverrides.find(slot.Name);
				if (iter != component.m_SlotOverrides.end() && iter->second.pMaterial != nullptr)
					++overridden_count;
			}

			ImGui::Spacing();
			ImGui::TextColored(EditorTheme::Token::TextLabel, "Materials");
			ImGui::SameLine();
			ImGui::TextDisabled("(%d slots, %d overridden)", static_cast<int>(slots.size()), overridden_count);

			for (int i = 0; i < static_cast<int>(slots.size()); ++i)
			{
				ImGui::PushID(i);
				changed |= DrawMaterialSlotRow(component, model, i);
				ImGui::PopID();
			}

			return changed;
		}

		bool DrawModelBlock(void* raw)
		{
			auto& component = *static_cast<ModelComponent*>(raw);
			auto& model = component.m_Model;
			bool changed = false;

			/* 名称与路径：内建模型显示身份名（BuiltinSphere 等）——它不是磁盘上的
			 * 文件，拿去做 RELATIVE_PATH 只会得到一串上跳相对路径。 */
			std::string model_name;
			std::string model_path;
			if (model != nullptr)
			{
				BuiltinModelType builtin_type{};
				if (model->TryGetBuiltinType(builtin_type))
				{
					model_name = model->GetPath();
					model_path = model_name;
				}
				else
				{
					model_name = PathToUtf8(PathFromUtf8(model->GetPath()).filename());
					model_path = RELATIVE_PATH(model->GetPath());
				}
			}

			/* 模型引用输入框：只读展示模型名，拖放 .mesh 直接落在输入框上；
			 * 完整路径在 tooltip 里给出（输入框只放名字，长路径不进布局） */
			const float value_width = ImGuiExt::BeginPropertyRow("Model");
			const float frame_height = ImGui::GetFrameHeight();
			const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
			const float clear_width = model != nullptr
				? ImGui::CalcTextSize("Clear").x + ImGui::GetStyle().FramePadding.x * 2.0f
				: 0.0f;
			const float target_width = model != nullptr
				? std::max(1.0f, value_width - clear_width - gap)
				: value_width;

			DrawRefInputBox("##ModelRef", model_name, target_width, "Drop .mesh here");

			if (ImGui::IsItemHovered())
			{
				if (model != nullptr)
					ImGui::SetTooltip("%s\n\nDrag a .mesh asset from the Resource Browser to assign or replace the model",
						model_path.c_str());
				else
					ImGui::SetTooltip("Drag a .mesh asset from the Resource Browser to assign the model");
			}

			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RESOURCE_BROWSER_ITEM"))
				{
					std::filesystem::path relative_path;
					if (payload->DataSize > 1 && TryPathFromUtf8Payload(
						payload->Data, static_cast<size_t>(payload->DataSize), relative_path))
					{
						std::string extension = PathToUtf8(relative_path.extension());
						std::transform(extension.begin(), extension.end(), extension.begin(),
							[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

						if (extension == ".mesh")
						{
							const std::string absolute_path = PathToUtf8(g_AssetsPath / relative_path);
							if (model == nullptr || model->GetPath() != absolute_path)
							{
								SharedPtr<Model> loaded_model = Model::Create(absolute_path);
								if (loaded_model != nullptr)
								{
									model = std::move(loaded_model);
									changed = true;
								}
							}
						}
					}
				}
				ImGui::EndDragDropTarget();
			}

			if (model != nullptr)
			{
				ImGui::SameLine(0.0f, gap);
				if (ImGui::Button("Clear", ImVec2(clear_width, frame_height)))
				{
					model.reset();
					changed = true;
				}
			}

			ImGuiExt::EndPropertyRow();

			/* 材质槽区块：每槽一行 + 当前材质属性（可就地编辑） */
			if (model != nullptr)
				changed |= DrawMaterialSlots(component, *model);

			return changed;
		}

		/* 光源类型决定持有哪个 Light 对象，切换时必须重建；
		 * 颜色 / 强度 / 投影这些公共属性沿用切换前的值。 */
		FieldAccessor MakeLightTypeAccessor()
		{
			FieldAccessor accessor;
			accessor.ValueSize = sizeof(int);

			accessor.Get = [](const void* component, void* out_value)
			{
				const auto& light = static_cast<const LightComponent*>(component)->m_Light;
				if (light != nullptr)
					*static_cast<int*>(out_value) = static_cast<int>(light->GetLightType());
			};

			accessor.Set = [](void* component, const void* in_value)
			{
				auto& light_component = *static_cast<LightComponent*>(component);
				const auto type = static_cast<LightType>(*static_cast<const int*>(in_value));

				if (light_component.m_Light != nullptr && light_component.m_Light->GetLightType() == type)
					return;

				glm::vec4 color{ 1.0f };
				float intensity = 1.0f;
				bool cast_shadow = false;
				/* 点光 / 聚光的共有范围：旧光有则沿用；旧光没有（方向光）则用新光的默认值 */
				float range = 0.0f;
				bool has_range = false;
				/* 阴影配置（尺寸 / 偏移 / 级联这些调过的值）随对象重建沿用 */
				SharedPtr<ShadowMapInfo> shadow_info;
				if (light_component.m_Light != nullptr)
				{
					color = light_component.m_Light->GetColor();
					intensity = light_component.m_Light->GetIntensity();
					cast_shadow = light_component.m_Light->IsCastShadow();
					shadow_info = light_component.m_Light->GetShadowMapInfo();
					if (const auto* punctual = AsPunctualLight(light_component.m_Light.get()))
					{
						range = punctual->GetRange();
						has_range = true;
					}
				}

				light_component.m_Type = type;
				light_component.m_Light = Light::Create(type);
				if (light_component.m_Light != nullptr)
				{
					light_component.m_Light->SetColor(color);
					light_component.m_Light->SetIntensity(intensity);
					light_component.m_Light->SetIsCastShadow(cast_shadow);
					if (auto* punctual = AsPunctualLight(light_component.m_Light.get()); punctual != nullptr && has_range)
						punctual->SetRange(range);
					if (shadow_info != nullptr && light_component.m_Light->GetShadowMapInfo() != nullptr)
						*light_component.m_Light->GetShadowMapInfo() = *shadow_info;
				}
			};

			return accessor;
		}

		/* ============ 点光 / 聚光的共有与专有参数 ============
		 * Range 长在 PunctualLight、Angle 长在 SpotLight，Light 基类不认识，所以手写访问器按类型取值：
		 * 类型不符时读保持缓冲、写不做任何事（配合字段的条件，两者都不会被触发）。 */

		/* 点光 / 聚光才有的参数 */
		bool IsPunctualLightComponent(const void* component)
		{
			const auto& light = static_cast<const LightComponent*>(component)->m_Light;
			return light != nullptr && AsPunctualLight(light.get()) != nullptr;
		}

		/* 聚光才有的参数（锥角） */
		bool IsSpotLightComponent(const void* component)
		{
			const auto& light = static_cast<const LightComponent*>(component)->m_Light;
			return light != nullptr && light->GetLightType() == LightType::Spot;
		}

		/* 光照范围（点光 / 聚光） */
		FieldAccessor MakeLightRangeAccessor()
		{
			FieldAccessor accessor;
			accessor.ValueSize = sizeof(float);

			accessor.Get = [](const void* component, void* out_value)
			{
				const auto& light = static_cast<const LightComponent*>(component)->m_Light;
				if (light == nullptr)
					return;
				if (const auto* punctual = AsPunctualLight(light.get()))
					*static_cast<float*>(out_value) = punctual->GetRange();
			};

			accessor.Set = [](void* component, const void* in_value)
			{
				const auto& light = static_cast<LightComponent*>(component)->m_Light;
				if (light == nullptr)
					return;
				if (auto* punctual = AsPunctualLight(light.get()))
					punctual->SetRange(*static_cast<const float*>(in_value));
			};

			return accessor;
		}

		/* 外锥全角（度；聚光），编辑时夹取到与渲染端一致的安全范围 */
		FieldAccessor MakeSpotAngleAccessor()
		{
			FieldAccessor accessor;
			accessor.ValueSize = sizeof(float);

			accessor.Get = [](const void* component, void* out_value)
			{
				const auto& light = static_cast<const LightComponent*>(component)->m_Light;
				if (light == nullptr || light->GetLightType() != LightType::Spot)
					return;
				*static_cast<float*>(out_value) = static_cast<const SpotLight*>(light.get())->GetAngle();
			};

			accessor.Set = [](void* component, const void* in_value)
			{
				const auto& light = static_cast<LightComponent*>(component)->m_Light;
				if (light == nullptr || light->GetLightType() != LightType::Spot)
					return;
				auto* spot_light = static_cast<SpotLight*>(light.get());
				spot_light->SetAngle(glm::clamp(*static_cast<const float*>(in_value), 1.0f, 170.0f));
			};

			return accessor;
		}

		/* ============ 阴影参数（ShadowMapInfo） ============
		 * 配置挂在 Light 的 SharedPtr<ShadowMapInfo> 上，用成员指针逐层解引用；光源或配置缺席时
		 * 读保持缓冲、写不做任何事。配置由 SetIsCastShadow(true) 备好，"勾选投影"之后字段立即可用。 */

		/* 勾选"投影"后才有可调的阴影参数（所有类型） */
		bool HasShadowSettings(const void* component)
		{
			const auto& light = static_cast<const LightComponent*>(component)->m_Light;
			return light != nullptr && light->IsCastShadow();
		}

		/* 级联数与阴影距离只对方向光有意义 */
		bool HasDirectionalShadowSettings(const void* component)
		{
			const auto& light = static_cast<const LightComponent*>(component)->m_Light;
			return light != nullptr && light->IsCastShadow() && light->GetLightType() == LightType::Directional;
		}

		/* ShadowMapInfo 标量字段的访问器；ClampMin / ClampMax 为编译期包络，
		 * 只约束写（渲染端另有兜底）：读到的总是实际存储值。
		 * 访问器是裸函数指针，无捕获，故包络走模板参数（C++20 浮点 NTTP）。 */
		template <auto Member, auto ConfigMember, double ClampMin = -1.0e30, double ClampMax = 1.0e30>
		FieldAccessor MakeShadowMapInfoAccessor()
		{
			using Value = typename Detail::MemberValue<decltype(ConfigMember)>::Type;

			FieldAccessor accessor;
			if constexpr (std::is_integral_v<Value>)
				accessor.ValueSize = sizeof(int);
			else
				accessor.ValueSize = sizeof(float);

			accessor.Get = [](const void* component, void* out_value)
			{
				const auto& light = static_cast<const LightComponent*>(component)->m_Light;
				if (light == nullptr)
					return;
				const auto& shadow_info = light->GetShadowMapInfo();
				if (shadow_info == nullptr)
					return;
				if constexpr (std::is_integral_v<Value>)
					*static_cast<int*>(out_value) = static_cast<int>(shadow_info.get()->*ConfigMember);
				else
					*static_cast<float*>(out_value) = static_cast<float>(shadow_info.get()->*ConfigMember);
			};

			accessor.Set = [](void* component, const void* in_value)
			{
				auto& light = static_cast<LightComponent*>(component)->m_Light;
				if (light == nullptr)
					return;
				const auto& shadow_info = light->GetShadowMapInfo();
				if (shadow_info == nullptr)
					return;
				if constexpr (std::is_integral_v<Value>)
				{
					const int clamped = static_cast<int>(
						std::clamp<double>(*static_cast<const int*>(in_value), ClampMin, ClampMax));
					shadow_info.get()->*ConfigMember = static_cast<Value>(clamped);
				}
				else
				{
					shadow_info.get()->*ConfigMember = static_cast<Value>(
						std::clamp<double>(*static_cast<const float*>(in_value), ClampMin, ClampMax));
				}
			};

			return accessor;
		}

		/* 嵌套对象为空时整个组件块不显示 */
		bool HasLightObject(const void* component)
		{
			return static_cast<const LightComponent*>(component)->m_Light != nullptr;
		}

		/* 探针的字段表之外的持久化内容：天空盒纹理与烘焙结果缓存路径。
		 * 对象为空时不写属性，读的时候也接受属性缺失。 */
		void SaveProbeExtra(tinyxml2::XMLElement* element, const void* component)
		{
			const auto& probe = static_cast<const ReflectionProbeComponent*>(component)->m_ReflectionProbe;
			if (probe == nullptr)
				return;

			const SharedPtr<DeviceTexture> skybox = probe->GetSkyBoxTexture();
			if (skybox != nullptr)
				element->SetAttribute("SkyBoxPath", RELATIVE_PATH(skybox->GetPath()).c_str());

			/* 缓存路径只在有烘焙结果时才落盘：否则场景会指向一份与当前状态不符的缓存 */
			const std::string& cache_path = probe->GetBakeCachePath();
			if (probe->IsBaked() && !cache_path.empty())
				element->SetAttribute("BakeCachePath", cache_path.c_str());
		}

		void LoadProbeExtra(const tinyxml2::XMLElement* element, void* component)
		{
			auto& probe = static_cast<ReflectionProbeComponent*>(component)->m_ReflectionProbe;
			if (probe == nullptr)
				return;

			/* 同上：天空盒路径按相对资产根存储，入口即转绝对 */
			if (const char* path = element->Attribute("SkyBoxPath"); path != nullptr && *path != '\0')
				probe->SetSkyBoxTexture(DeviceTexture::Create(ABSOLUTE_PATH(path)));

			/* 命中缓存就直接恢复烘焙结果：加载后无需再烘焙 */
			if (const char* cache_path = element->Attribute("BakeCachePath"); cache_path != nullptr && *cache_path != '\0')
			{
				probe->SetBakeCachePath(cache_path);
				if (!probe->LoadBakeCache(ABSOLUTE_PATH(cache_path)))
					CORE_LOG_WARN("ReflectionProbe: bake cache '{}' is unavailable, it will be baked again", cache_path);
			}
		}

		/* ==================== 烘焙结果详情 ====================
		 * 把「烘焙了什么 / 存到哪里 / 文件多大」摊开给用户看；大小和时间的格式化走 Utils 的共用实现。 */

		/* 一张烘焙贴图的规格行：尺寸 / 格式 / mip 层数（贴图不存在时不画这一行） */
		void DrawBakeImageRow(const char* label, const SharedPtr<DeviceTexture>& texture)
		{
			if (texture == nullptr)
				return;

			const TextureDesc& desc = texture->GetTextureDesc();
			const char* format_name = GetEnumName(desc.Format);
			const std::string value = std::to_string(desc.Width) + " x " + std::to_string(desc.Height)
				+ ", " + (format_name != nullptr ? format_name : "?")
				+ ", " + std::to_string(desc.MipLevels) + (desc.MipLevels > 1 ? " mips" : " mip");

			ImGuiExt::DrawCommonTextUI(label, value);
		}

		/* 缓存文件行：磁盘上的大小与写入时间；还没写出过时说明它随场景保存产生 */
		void DrawBakeCacheFileRow(const ReflectionProbe& probe)
		{
			const std::string& cache_path = probe.GetBakeCachePath();

			std::filesystem::path relative_path;
			if (cache_path.empty() || !TryPathFromUtf8(cache_path, relative_path))
			{
				ImGuiExt::DrawCommonTextUI("CacheFile", "not written yet");
				return;
			}

			const std::filesystem::path absolute_path = g_AssetsPath / relative_path;

			std::error_code error;
			const uintmax_t size = std::filesystem::file_size(absolute_path, error);
			if (error)
			{
				ImGuiExt::DrawCommonTextUI("CacheFile", "missing on disk");
				return;
			}

			ImGuiExt::DrawCommonTextUI("CacheFile",
				FormatFileSize(size) + ", " + FormatFileWriteTime(absolute_path));
		}

		/* 烘焙结果预览：三张十字展开图并排（立方图不能直接被 2D UI 采样，
		 * 探针把它按需转成 2D —— 见 ReflectionProbe::RequestBakePreview）。
		 * 首次显示 / Rebake 后自动请求生成；鼠标悬停放大看细节。 */
		void DrawBakePreviewRow(ReflectionProbe& probe)
		{
			using PreviewState = ReflectionProbe::BakePreviewState;

			/* 已烘焙但还没有预览 → 请求生成（幂等；生成由探针的状态机在渲染帧推进） */
			if (probe.GetBakePreviewState() == PreviewState::Idle
				&& !probe.HasBakePreview() && !probe.HasBakePreviewFailed())
			{
				probe.RequestBakePreview();
			}

			if (!probe.HasBakePreview())
			{
				if (probe.HasBakePreviewFailed())
					ImGuiExt::DrawCommonTextUI("Preview", "generation failed");
				else if (probe.GetBakePreviewState() != PreviewState::Idle)
					ImGuiExt::DrawCommonTextUI("Preview", "generating...");
				return;
			}

			struct PreviewEntry
			{
				const char* Label;
				ReflectionProbe::BakePreviewKind Kind;
			};
			constexpr PreviewEntry entries[] = {
				{ "Environment", ReflectionProbe::BakePreviewKind::Environment },
				{ "Irradiance",  ReflectionProbe::BakePreviewKind::Irradiance },
				{ "Prefilter",   ReflectionProbe::BakePreviewKind::Prefilter },
			};

			/* 只画实际存在的（烘焙配置可能省掉 IRF / PF） */
			struct ShownPreview
			{
				const char* Label;
				SharedPtr<DeviceTexture> Texture;
			};
			ShownPreview shown[3];
			size_t count = 0;
			for (const PreviewEntry& entry : entries)
			{
				SharedPtr<DeviceTexture> texture = probe.GetBakePreviewTexture(entry.Kind);
				if (texture != nullptr)
					shown[count++] = ShownPreview{ entry.Label, std::move(texture) };
			}

			ImGui::Spacing();

			const float spacing = ImGui::GetStyle().ItemSpacing.x;
			const float total_width = ImGui::GetContentRegionAvail().x;
			const float image_width = (total_width - spacing * static_cast<float>(count - 1)) / static_cast<float>(count);
			const float image_height = image_width * 0.75f; /* 十字展开是 4x3 */

			const float start_x = ImGui::GetCursorPosX();
			const float start_y = ImGui::GetCursorPosY();

			for (size_t index = 0; index < count; ++index)
			{
				if (index > 0)
					ImGui::SameLine(0.0f, spacing);

				/* UV 不翻转：预览图的第 0 行（+Y 面）就在顶部 —— 与图片的 (0,1)-(1,0)
				 * 相反，那些贴图在上传前被垂直翻转（IsFlipV），数据行序与这里正相反 */
				ImGui::Image((ImTextureID)shown[index].Texture.get(), ImVec2(image_width, image_height),
					ImVec2(0, 0), ImVec2(1, 1));

				if (ImGui::IsItemHovered())
				{
					ImGui::BeginTooltip();
					ImGui::Image((ImTextureID)shown[index].Texture.get(), ImVec2(320.0f, 240.0f),
						ImVec2(0, 0), ImVec2(1, 1));
					ImGui::TextUnformatted(shown[index].Label);
					ImGui::EndTooltip();
				}
			}

			/* 名字逐张居中放在图的下面（宽度不足时以图为准，宁挤不偏） */
			const float caption_y = start_y + image_height + ImGui::GetStyle().ItemSpacing.y;
			for (size_t index = 0; index < count; ++index)
			{
				const float text_width = ImGui::CalcTextSize(shown[index].Label).x;
				const float center_x = start_x + static_cast<float>(index) * (image_width + spacing)
					+ (image_width - text_width) * 0.5f;

				ImGui::SetCursorPos(ImVec2(center_x, caption_y));
				ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
				ImGui::TextUnformatted(shown[index].Label);
				ImGui::PopStyleColor();
			}
		}

		/* 天空盒是资源引用，要能选择、悬停预览，所以整块自定义绘制；烘焙参数走 schema 字段。
		 * 返回值是"有没有改动"：换天空盒会生成快照命令（撤销和脏标记靠它）；Rebake 是异步操作、不算改动。 */
		bool DrawProbeBlock(void* raw)
		{
			auto& probe = static_cast<ReflectionProbeComponent*>(raw)->m_ReflectionProbe;
			if (probe == nullptr)
				return false;

			bool changed = false;

			SharedPtr<DeviceTexture> skybox = probe->GetSkyBoxTexture();
			ImGuiExt::DrawTextureUI("SkyBox", skybox);
			if (skybox != probe->GetSkyBoxTexture())
			{
				probe->SetSkyBoxTexture(skybox);
				changed = true;
			}

			ImGui::Separator();

			/* 烘焙结果是只读状态：它随「保存场景」一并落盘，写盘要等 GPU 执行完，
			 * 所以不是存档当帧就完成。状态行与上面的资源行共用同一套属性行布局。 */
			using WriteState = ReflectionProbe::BakeCacheWriteState;
			const WriteState state = probe->GetBakeCacheWriteState();

			const char* status = "Not baked";
			if (state == WriteState::Requested)
				status = "Baking...";
			else if (state == WriteState::WaitingForGPU)
				status = "Saving...";
			else if (probe->IsBaked())
				status = "Baked";

			ImGuiExt::DrawCommonTextUI("Status", status);

			if (state == WriteState::Idle && probe->IsBaked())
			{
				const std::string& cache_path = probe->GetBakeCachePath();
				ImGuiExt::DrawCommonTextUI("BakeCache",
					cache_path.empty() ? "(written with the scene)" : cache_path);

				/* 详情：三张烘焙贴图的规格（+ 预览）+ 缓存文件在磁盘上的大小与写入时间 */
				DrawBakeImageRow("Environment", probe->GetEnvCubemap());
				DrawBakeImageRow("Irradiance", probe->GetIrradianceMap());
				DrawBakeImageRow("Prefilter", probe->GetPrefilterMap());
				DrawBakePreviewRow(*probe);
				DrawBakeCacheFileRow(*probe);
			}

			/* 写盘失败不隐藏：否则用户会以为已经存下来了 */
			if (probe->HasBakeCacheWriteFailed())
				ImGuiExt::DrawCommonTextUI("BakeCache", "write failed");

			/* 重烘焙走"请求"：本帧绘制列表还画着预览图，当场 Reset 会把它们
			 * 销毁成悬垂纹理指针（渲染时崩）；实际重置推迟到下一帧 Tick */
			if (state == WriteState::Idle && ImGui::Button("Rebake"))
				probe->RequestRebake();

			return changed;
		}

		/* 探针为空时整个组件块不显示 */
		bool HasProbeObject(const void* component)
		{
			return static_cast<const ReflectionProbeComponent*>(component)->m_ReflectionProbe != nullptr;
		}

		/* 用组件类型生成带访问器与数据快照的注册器 */
		template <typename T>
		ComponentRegistrar MakeRegistrar(const char* name)
		{
			ComponentRegistrar registrar(name, std::type_index(typeid(T)));
			registrar.Accessors(
				[](Entity& e) -> bool { return e.HasComponent<T>(); },
				[](Entity& e) -> void* { return static_cast<void*>(&e.GetComponent<T>()); },
				[](Entity& e) { e.RemoveComponent<T>(); },
				[](Entity& e) { e.AddComponent<T>(); });
			registrar.Snapshot(
				[](Entity& e, ComponentSnapshot& out)
				{
					if (e.HasComponent<T>())
						out = ComponentSnapshot(e.GetComponent<T>());
				},
				[](Entity& e, const ComponentSnapshot& in)
				{
					const T* source = in.As<T>();
					if (source == nullptr)
						return;

					if (e.HasComponent<T>())
					{
						e.GetComponent<T>() = *source;
						return;
					}

					/* 按快照直接构造：组件的 OnAdded 会看到最终数据，
					 * 需要把自身注册到场景的组件（如反射探针）才不会挂错对象。 */
					e.AddComponent<T>(*source);
				});
			return registrar;
		}

		/* ==================== 内置组件注册 ====================
		 * 新增组件在这补一段就行，Inspector 和 Add 菜单自动生效。注册对象靠 ComponentRegistry 同 TU
		 * 被引用而链进来（静态库不会丢弃本文件）。 */
		struct BuiltinComponentRegistration
		{
			BuiltinComponentRegistration()
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

				MakeRegistrar<SpriteComponent>("Sprite")
					.SaveExtra(&SaveSpriteTexture)
					.LoadExtra(&LoadSpriteTexture)
					.Field(&SpriteComponent::m_BaseColor, "BaseColor", FieldType::Color)
					.Field(&SpriteComponent::m_TilingFactor, "TilingFactor", FieldType::Float, 0.1f, 1.0f)
					/* 未设置贴图时 TilingFactor 无意义，条件不满足则不显示（值保留） */
					.When(FieldCondition{ 0, FieldType::Bool, ConditionOp::IsTrue, 0.0,
						[](const void* component)
						{
							return static_cast<const SpriteComponent*>(component)->m_Texture != nullptr;
						} })
					.CustomDraw(&DrawSpriteBlock)
					.Register();

				MakeRegistrar<CameraComponent>("Camera")
					.Field(&CameraComponent::m_IsPrimary, "IsPrimary", FieldType::Bool)
					.Field(&CameraComponent::m_IsFixedAspectRatio, "IsFixedAspectRatio", FieldType::Bool)
					/* 投影参数在嵌套的 Camera 对象里，经访问器读写 */
					.Field(MakeEnumAccessor<&CameraComponent::m_Camera,
						&Camera::GetProjectionType, &Camera::SetProjectionType>(),
						"ProjectionType", FieldType::Enum)
					.EnumOf<CameraProjectionType>()
					.Field(MakeAccessor<&CameraComponent::m_Camera, &Camera::GetFov, &Camera::SetFov>(),
						"Fov", FieldType::Float)
					.When(FieldCondition{ 0, FieldType::Bool, ConditionOp::IsTrue, 0.0, &IsPerspectiveProjection })
					.Field(MakeAccessor<&CameraComponent::m_Camera, &Camera::GetHeightSize, &Camera::SetHeightSize>(),
						"HeightSize", FieldType::Float)
					.When(FieldCondition{ 0, FieldType::Bool, ConditionOp::IsTrue, 0.0, &IsOrthographicProjection })
					.Field(MakeAccessor<&CameraComponent::m_Camera, &Camera::GetNearClip, &Camera::SetNearClip>(),
						"NearClip", FieldType::Float)
					.SerializeName("Near")
					.Field(MakeAccessor<&CameraComponent::m_Camera, &Camera::GetFarClip, &Camera::SetFarClip>(),
						"FarClip", FieldType::Float)
					.SerializeName("Far")
					.Register();

				MakeRegistrar<ModelComponent>("Model")
					.SaveExtra(&SaveModelPath)
					.LoadExtra(&LoadModelPath)
					.CustomDraw(&DrawModelBlock)
					.Register();

				/* 光源对象由构造参数决定，故逐类声明可添加形态；菜单名即光源类型名。 */
				MakeRegistrar<LightComponent>("Light")
					.Field(MakeLightTypeAccessor(), "Type", FieldType::Enum)
					.EnumOf<LightType>()
					.SerializeName("LightType")
					.Field(MakeAccessor<&LightComponent::m_Light, &Light::GetColor, &Light::SetColor>(),
						"Color", FieldType::Color)
					.SerializeName("LightColor")
					.Field(MakeAccessor<&LightComponent::m_Light, &Light::GetIntensity, &Light::SetIntensity>(),
						"Intensity", FieldType::Float)
					.SerializeName("LightIntensity")
					.Field(MakeAccessor<&LightComponent::m_Light, &Light::IsCastShadow, &Light::SetIsCastShadow>(),
						"CastShadow", FieldType::Bool)
					.SerializeName("IsCastShadow")
					/* 范围与锥角：条件同时驱动 UI（不适用类型不显示）与序列化（不适用不写出） */
					.Field(MakeLightRangeAccessor(), "Range", FieldType::Float)
					.When(FieldCondition{ 0, FieldType::Bool, ConditionOp::Always, 0.0, &IsPunctualLightComponent })
					.SerializeName("LightRange")
					.Field(MakeSpotAngleAccessor(), "Angle", FieldType::Float)
					.When(FieldCondition{ 0, FieldType::Bool, ConditionOp::Always, 0.0, &IsSpotLightComponent })
					.SerializeName("LightAngle")
					/* 阴影参数：勾选投影后出现（配置由 SetIsCastShadow 备好）；
					 * 级联数与阴影距离只对方向光有意义。条件同时驱动 UI 与序列化。 */
					.Field(MakeShadowMapInfoAccessor<&LightComponent::m_Light, &ShadowMapInfo::Size, 64.0, 8192.0>(),
						"Size", FieldType::Int, 8.0f)
					.When(FieldCondition{ 0, FieldType::Bool, ConditionOp::Always, 0.0, &HasShadowSettings })
					.SerializeName("ShadowSize")
					.Field(MakeShadowMapInfoAccessor<&LightComponent::m_Light, &ShadowMapInfo::ConstantBias>(),
						"Bias", FieldType::Float, 0.001f)
					.When(FieldCondition{ 0, FieldType::Bool, ConditionOp::Always, 0.0, &HasShadowSettings })
					.SerializeName("ShadowBias")
					.Field(MakeShadowMapInfoAccessor<&LightComponent::m_Light, &ShadowMapInfo::CascadeCnt, 1.0, 4.0>(),
						"CascadeCount", FieldType::Int)
					.When(FieldCondition{ 0, FieldType::Bool, ConditionOp::Always, 0.0, &HasDirectionalShadowSettings })
					.SerializeName("CascadeCount")
					.Field(MakeShadowMapInfoAccessor<&LightComponent::m_Light, &ShadowMapInfo::ShadowFar, 0.0, 1.0e30>(),
						"ShadowFar", FieldType::Float, 1.0f)
					.When(FieldCondition{ 0, FieldType::Bool, ConditionOp::Always, 0.0, &HasDirectionalShadowSettings })
					.SerializeName("ShadowFar")
					.Visible(&HasLightObject)
					.Variant("Directional Light", [](Entity& entity)
						{
							entity.AddComponent<LightComponent>(LightType::Directional);
						})
					.Variant("Point Light", [](Entity& entity)
						{
							entity.AddComponent<LightComponent>(LightType::Point);
						})
					.Variant("Spot Light", [](Entity& entity)
						{
							entity.AddComponent<LightComponent>(LightType::Spot);
						})
					.Register();

				/* 反射探针：烘焙参数以结构体整体读写，故用配置访问器逐个字段暴露 */
				MakeRegistrar<ReflectionProbeComponent>("ReflectionProbe")
					.SaveExtra(&SaveProbeExtra)
					.LoadExtra(&LoadProbeExtra)
					.Field(MakeAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
							&ReflectionProbe::IsRealtime, &ReflectionProbe::SetRealtime>(),
						"Realtime", FieldType::Bool)
					.Field(MakeConfigAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
							&ReflectionProbe::GetBakeConfig, &ReflectionProbe::SetBakeConfig,
							&ReflectionProbe::BakeConfig::EnvSize>(),
						"EnvSize", FieldType::Int)
					.Field(MakeConfigAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
							&ReflectionProbe::GetBakeConfig, &ReflectionProbe::SetBakeConfig,
							&ReflectionProbe::BakeConfig::IrradianceSize>(),
						"IrradianceSize", FieldType::Int)
					.Field(MakeConfigAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
							&ReflectionProbe::GetBakeConfig, &ReflectionProbe::SetBakeConfig,
							&ReflectionProbe::BakeConfig::PrefilterSize>(),
						"PrefilterSize", FieldType::Int)
					.Field(MakeConfigAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
							&ReflectionProbe::GetBakeConfig, &ReflectionProbe::SetBakeConfig,
							&ReflectionProbe::BakeConfig::PrefilterMipLevels>(),
						"PrefilterMipLevels", FieldType::Int)
					.Field(MakeConfigAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
							&ReflectionProbe::GetBakeConfig, &ReflectionProbe::SetBakeConfig,
							&ReflectionProbe::BakeConfig::BakeSkyBoxOnly>(),
						"BakeSkyBoxOnly", FieldType::Bool)
					.Field(MakeConfigAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
							&ReflectionProbe::GetBakeConfig, &ReflectionProbe::SetBakeConfig,
							&ReflectionProbe::BakeConfig::BakeDiffuse>(),
						"BakeDiffuse", FieldType::Bool)
					.Field(MakeConfigAccessor<&ReflectionProbeComponent::m_ReflectionProbe,
							&ReflectionProbe::GetBakeConfig, &ReflectionProbe::SetBakeConfig,
							&ReflectionProbe::BakeConfig::BakeSpecular>(),
						"BakeSpecular", FieldType::Bool)
					.CustomDraw(&DrawProbeBlock)
					.Visible(&HasProbeObject)
					.Register();
			}
		};

		const BuiltinComponentRegistration s_BuiltinComponentRegistration;
	}
}
