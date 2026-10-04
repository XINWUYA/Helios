#include "Pch.h"
#include "SceneHierarchy.h"
#include "EditorIcons.h"
#include "PanelRegistry.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include "EntityTemplateRegistry.h"
#include "Command/ComponentFieldCommand.h"
#include "Command/AddComponentCommand.h"
#include "Command/RemoveComponentCommand.h"
#include "Command/CreateEntityCommand.h"
#include "Command/DeleteEntityCommand.h"
#include <cstring>

namespace Helios
{
	SceneHierarchy::SceneHierarchy()
	{
		PROFILE_FUNCTION();
	}

	SceneHierarchy::SceneHierarchy(const SharedPtr<Scene>& scene)
	{
		PROFILE_FUNCTION();

		SetOwnerScene(scene);
	}

	void SceneHierarchy::SetOwnerScene(const SharedPtr<Scene>& scene)
	{
		m_pOwnerScene = scene;
		m_SelectedEntity = {};

		/* 场景已更换，字段编辑的合并窗口不再有效 */
		m_FieldEditTransactionOpen = false;
	}

	void SceneHierarchy::OnImGuiRender()
	{
		PROFILE_FUNCTION();

		/* 场景实体列表 */
		ShowSceneHierarchyUI();

		/* 实体属性 */
		ShowEntityPropertiesUI();
	}

	void SceneHierarchy::SetSelectedEntity(const Entity& entity)
	{
		m_SelectedEntity = entity;
	}

	void SceneHierarchy::ShowSceneHierarchyUI()
	{
		PROFILE_FUNCTION();

		/* 删除请求延后到遍历结束后执行：在 each 中销毁当前实体并不安全 */
		Entity pending_delete{};

		ImGui::Begin(Panel::kSceneHierarchy);
		{
			m_pOwnerScene->GetRegistry().each(
				[&](auto entity_id)
				{
					Entity entity{ entity_id, m_pOwnerScene };
					if (ShowEntityNode(entity) && !pending_delete)
						pending_delete = entity;
				});

			/* 空白处右键，唤出新建（条目来自实体预设注册表） */
			if (ImGui::BeginPopupContextWindow("New", 1, false))
			{
				if (ImGui::BeginMenu("New A Entity"))
				{
					for (const EntityTemplateDesc& template_desc : EntityTemplateRegistry::Instance().All())
					{
						if (ImGui::MenuItem(template_desc.Name))
							m_SelectedEntity = CreateEntityFromTemplate(template_desc);
					}

					ImGui::EndMenu();
				}
				ImGui::EndPopup();
			}
		}
		ImGui::End();

		/* 删除经命令栈落地，可撤销 */
		if (pending_delete)
			DeleteEntity(pending_delete);
	}

	/* 新建实体：走命令栈则实体本身也进历史（撤销即消失） */
	Entity SceneHierarchy::CreateEntityFromTemplate(const EntityTemplateDesc& template_desc)
	{
		if (m_pOwnerScene == nullptr)
			return {};

		if (m_pCommandStack == nullptr)
		{
			Entity entity = m_pOwnerScene->CreateEntity(template_desc.Name);
			for (const AddFunc add : template_desc.Components)
			{
				if (add != nullptr)
					add(entity);
			}
			return entity;
		}

		auto command = CreateUniquePtr<CreateEntityCommand>(m_pOwnerScene, template_desc.Name, template_desc.Components);
		CreateEntityCommand* raw = command.get();
		m_pCommandStack->Execute(std::move(command));
		return raw->GetEntity();
	}

	void SceneHierarchy::DeleteEntity(Entity entity)
	{
		if (!entity || m_pOwnerScene == nullptr)
			return;

		if (m_pCommandStack == nullptr)
		{
			m_pOwnerScene->DestroyEntity(entity);
		}
		else
		{
			m_pCommandStack->Execute(CreateUniquePtr<DeleteEntityCommand>(m_pOwnerScene, entity));
		}

		if (m_SelectedEntity == entity)
			m_SelectedEntity = {};
	}

	void SceneHierarchy::ShowEntityPropertiesUI()
	{
		PROFILE_FUNCTION();

		ImGui::Begin(Panel::kProperties);
		{
			if (m_SelectedEntity)
				ShowEntityComponents();
		}
		ImGui::End();
	}

	/* 返回 true 表示用户请求删除该实体（由调用方在遍历结束后执行） */
	bool SceneHierarchy::ShowEntityNode(Entity& entity)
	{
		PROFILE_FUNCTION();

		const auto& name = entity.GetComponent<NameComponent>().m_Name;

		ImGuiTreeNodeFlags flags = ((m_SelectedEntity == entity) ? ImGuiTreeNodeFlags_Selected : 0) | ImGuiTreeNodeFlags_OpenOnArrow;
		flags |= ImGuiTreeNodeFlags_SpanAvailWidth;
		const bool is_opened = ImGui::TreeNodeEx((void*)(uint64_t)(uint32_t)entity, flags, name.c_str());

		if (ImGui::IsItemClicked())
		{
			m_SelectedEntity = entity;
		}

		/* 右键选择删除当前节点 */
		bool entity_deleted = false;
		if (ImGui::BeginPopupContextItem())
		{
			if (ImGui::MenuItem("Delete"))
				entity_deleted = true;

			ImGui::EndPopup();
		}

		/* 展开子节点 */
		if (is_opened)
		{
			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
			if (bool child_opened = ImGui::TreeNodeEx((void*)9817239, flags, name.c_str()))
				ImGui::TreePop();
			ImGui::TreePop();
		}

		return entity_deleted;
	}

	/* 按字段类型绘制单个控件，field_ptr 指向可写的字段存储 */
	static void DrawFieldControl(const FieldDesc& field, void* field_ptr)
	{
		switch (field.Type)
		{
		case FieldType::Bool:
			ImGuiExt::DrawCheckboxUI(field.Name, *reinterpret_cast<bool*>(field_ptr));
			break;
		case FieldType::Int:
			ImGuiExt::DrawDragIntUI(field.Name, *reinterpret_cast<int*>(field_ptr));
			break;
		case FieldType::Float:
			ImGuiExt::DrawDragFloatUI(field.Name, *reinterpret_cast<float*>(field_ptr));
			break;
		case FieldType::Vec2:
			ImGuiExt::DrawDragFloat2UI(field.Name, *reinterpret_cast<glm::vec2*>(field_ptr));
			break;
		case FieldType::Vec3:
		{
			auto& vec = *reinterpret_cast<glm::vec3*>(field_ptr);
			if (field.Semantics != nullptr && std::strcmp(field.Semantics, "AngleDeg") == 0)
			{
				/* 内部以弧度存储、Inspector 按角度编辑 */
				glm::vec3 degree = glm::degrees(vec);
				ImGuiExt::DrawVec3ControlUI(field.Name, degree, field.ResetValue);
				vec = glm::radians(degree);
			}
			else
			{
				ImGuiExt::DrawVec3ControlUI(field.Name, vec, field.ResetValue);
			}
			break;
		}
		case FieldType::Vec4:
		case FieldType::Color:
			ImGuiExt::DrawColorUI(field.Name, *reinterpret_cast<glm::vec4*>(field_ptr));
			break;
		case FieldType::Enum:
		{
			if (field.GetEnumNames == nullptr)
				break;

			/* 枚举底层类型可能小于 int（如 uint8_t），按实际大小读写 */
			const size_t size = (field.ValueSize == 0 || field.ValueSize > sizeof(int))
				? sizeof(int) : field.ValueSize;
			int value = 0;
			std::memcpy(&value, field_ptr, size);
			ImGuiExt::DrawComboUI(field.Name, field.GetEnumNames(), value);
			std::memcpy(field_ptr, &value, size);
			break;
		}
		case FieldType::String:
		{
			auto& str = *reinterpret_cast<std::string*>(field_ptr);
			char buffer[256] = {};
			std::strncpy(buffer, str.c_str(), sizeof(buffer));
			ImGui::Text("%s:", field.Name);
			ImGui::SameLine();
			if (ImGui::InputText("##value", buffer, sizeof(buffer)))
				str = std::string(buffer);
			break;
		}
		default:
			break;
		}
	}

	namespace
	{
		/* 字段值的字节数（定长字段由注册表给出） */
		size_t FieldByteSize(const FieldDesc& field)
		{
			return (field.ValueSize != 0) ? field.ValueSize : sizeof(float);
		}

		/* 访问器字段经 getter/setter 读写，偏移字段直接取址 */
		bool IsAccessorField(const FieldDesc& field)
		{
			return field.Get != nullptr && field.Set != nullptr;
		}
	}

	/* 按字段元数据生成控件；编辑前后各取一次值，有变化则生成字段改动命令 */
	void SceneHierarchy::DrawComponentFieldsBySchema(const ComponentDesc& desc, Entity& entity, void* component)
	{
		/* 上一帧有控件活跃、这一帧没有 → 一次编辑结束，给命令栈封口。
		 * 否则下一次编辑会并进上一条历史，撤销一次退过头。 */
		const bool item_active = ImGui::IsAnyItemActive();
		if (m_FieldEditTransactionOpen && !item_active)
		{
			m_FieldEditTransactionOpen = false;
			if (m_pCommandStack != nullptr)
				m_pCommandStack->EndTransaction();
		}

		for (const FieldDesc& field : desc.Fields)
		{
			/* 条件不满足：不画该行（字段值保留，序列化照常） */
			if (!EvaluateCondition(field.Condition, component))
				continue;

			ImGui::PushID(field.Name);

			const size_t value_size = FieldByteSize(field);
			const bool is_text = (field.Type == FieldType::String);
			const bool is_accessor = IsAccessorField(field);

			/* 编辑前的值，以及供控件操作的存储 */
			alignas(16) uint8_t before[kFieldValueCapacity] = {};
			alignas(16) uint8_t edited[kFieldValueCapacity] = {};
			std::string before_text;

			void* field_ptr = nullptr;
			if (is_accessor)
			{
				field.Get(component, edited);
				std::memcpy(before, edited, value_size);
				field_ptr = edited;
			}
			else
			{
				field_ptr = static_cast<uint8_t*>(component) + field.Offset;
				if (is_text)
					before_text = *static_cast<const std::string*>(field_ptr);
				else
					std::memcpy(before, field_ptr, value_size);
			}

			DrawFieldControl(field, field_ptr);

			/* 访问器字段：把编辑结果写回对象 */
			if (is_accessor)
				field.Set(component, field_ptr);

			/* 只有用户正在操作控件时才生成命令：外部改动（如 Gizmo 拖拽、
			 * 组件被移除）不是字段编辑，不应产生历史 */
			if (m_pCommandStack != nullptr && ImGui::IsAnyItemActive())
			{
				bool changed = false;
				if (is_text)
					changed = (before_text != *static_cast<const std::string*>(field_ptr));
				else
					changed = (std::memcmp(before, field_ptr, value_size) != 0);

				if (changed)
				{
					if (!m_FieldEditTransactionOpen)
					{
						m_FieldEditTransactionOpen = true;
						m_pCommandStack->BeginTransaction();
					}

					if (is_text)
					{
						m_pCommandStack->Execute(CreateUniquePtr<ComponentFieldCommand>(
							entity, desc, field, before_text, *static_cast<const std::string*>(field_ptr)));
					}
					else
					{
						m_pCommandStack->Execute(CreateUniquePtr<ComponentFieldCommand>(
							entity, desc, field, before, field_ptr));
					}
				}
			}

			ImGui::PopID();
		}
	}

	/* 绘制单个组件块（折叠标题 + schema 字段 + 自定义绘制 + 移除菜单） */
	void SceneHierarchy::DrawComponentBlock(const ComponentDesc& desc, Entity& entity, void* component)
	{
		const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowItemOverlap | ImGuiTreeNodeFlags_FramePadding;
		const float panel_width = ImGui::GetContentRegionAvail().x;

		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{ 4, 4 });
		ImGui::Separator();
		const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(desc.Type.hash_code()), flags, "%s", desc.Name);
		ImGui::PopStyleVar();

		/* 设置图标 */
		START_TRANSPARENT_BUTTON;
		START_STYLE_ALPHA(0.5f);
		ImGui::SameLine(panel_width - 15);
		if (Icons::IconButton(Icons::Id::Menu, ImVec2(20, 20)))
			ImGui::OpenPopup("ComponentSettings");
		END_STYLE_ALPHA;
		END_TRANSPARENT_BUTTON;

		/* 删除组件选项 */
		bool remove = false;
		if (ImGui::BeginPopup("ComponentSettings"))
		{
			if (ImGui::MenuItem("Remove"))
				remove = true;

			ImGui::EndPopup();
		}

		/* 展开时显示组件内容 */
		if (open)
		{
			DrawComponentFieldsBySchema(desc, entity, component);
			if (desc.CustomDraw != nullptr)
				desc.CustomDraw(component);
			ImGui::TreePop();
		}

		/* 删除组件：经命令栈则可连组件数据一起还原 */
		if (remove && desc.Remove != nullptr)
		{
			if (m_pCommandStack != nullptr)
			{
				m_pCommandStack->Execute(CreateUniquePtr<RemoveComponentCommand>(entity, desc));
			}
			else
			{
				desc.Remove(entity);
			}
		}
	}

	void SceneHierarchy::ShowEntityComponents()
	{
		PROFILE_FUNCTION();

		/* 增加组件按钮 */
		const float panel_width = ImGui::GetContentRegionAvail().x; /* 窗口区域宽度 */
		ImGui::SameLine(panel_width - 15); /* 放置在同行靠右的位置 */
		ShowAddComponentButton();

		/* 全部组件：由 ComponentRegistry 驱动，本类不认识任何具体组件类型。
		 * 新增组件只需在注册表补一段，这里零改动。 */
		for (const ComponentDesc& desc : ComponentRegistry::Instance().All())
		{
			if (desc.Has == nullptr || !desc.Has(m_SelectedEntity))
				continue;

			void* component = (desc.GetPtr != nullptr) ? desc.GetPtr(m_SelectedEntity) : nullptr;
			if (component == nullptr)
				continue;

			/* 组件级条件：如持有对象为空则整块不显示 */
			if (desc.Visible != nullptr && !desc.Visible(component))
				continue;

			ImGui::PushID(static_cast<int>(desc.Type.hash_code()));
			DrawComponentBlock(desc, m_SelectedEntity, component);
			ImGui::PopID();
		}
	}

	/* 增加组件按钮：菜单项来自注册表（bAddable == false 的组件不出现，如 Name / Transform） */
	void SceneHierarchy::ShowAddComponentButton()
	{
		PROFILE_FUNCTION();

		START_STYLE_ALPHA(0.5f);
		if (Icons::IconButton(Icons::Id::Add, ImVec2(20, 20)))
			ImGui::OpenPopup("AddComponentPopup");
		END_STYLE_ALPHA;

		if (ImGui::BeginPopup("AddComponentPopup"))
		{
			for (const ComponentDesc& desc : ComponentRegistry::Instance().All())
			{
				if (!desc.bAddable)
					continue;

				for (const ComponentVariant& variant : desc.Variants)
				{
					if (variant.Add == nullptr)
						continue;

					const char* label = (variant.MenuName != nullptr) ? variant.MenuName : desc.Name;
					if (ImGui::MenuItem(label))
					{
						if (m_pCommandStack != nullptr)
						{
							m_pCommandStack->Execute(CreateUniquePtr<AddComponentCommand>(
								m_SelectedEntity, desc.Name, variant.Add, desc.Remove));
						}
						else
						{
							variant.Add(m_SelectedEntity);
						}

						ImGui::CloseCurrentPopup();
					}
				}
			}

			ImGui::EndPopup();
		}
	}
}
