#include "Pch.h"
#include "SceneHierarchy.h"
#include "EditorIcons.h"
#include "PanelRegistry.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include "Helios/Reflection/EntityTemplateRegistry.h"
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

		ImGui::Begin(Panel::kSceneHierarchy);
		{
			m_pOwnerScene->GetRegistry().each(
				[&](auto entity_id)
				{
					Entity entity{ entity_id, m_pOwnerScene };
					ShowEntityNode(entity);
				});

			/* 点击左键，选中 */
			// if (ImGui::IsMouseDown(0) && ImGui::IsWindowHovered())
			// 	m_SelectedEntity = {};

			/* 空白处右键，唤出新建（条目来自实体预设注册表） */
			if (ImGui::BeginPopupContextWindow("New", 1, false))
			{
				if (ImGui::BeginMenu("New A Entity"))
				{
					for (const EntityTemplateDesc& template_desc : EntityTemplateRegistry::Instance().All())
					{
						if (ImGui::MenuItem(template_desc.Name))
						{
							Entity entity = m_pOwnerScene->CreateEntity(template_desc.Name);
							for (const AddFunc add : template_desc.Components)
							{
								if (add != nullptr)
									add(entity);
							}
							m_SelectedEntity = entity;
						}
					}

					ImGui::EndMenu();
				}
				ImGui::EndPopup();
			}
		}
		ImGui::End();
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

	void SceneHierarchy::ShowEntityNode(Entity& entity)
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

		/* 确认删除节点 */
		if (entity_deleted)
		{
			m_pOwnerScene->DestroyEntity(entity);
			if (m_SelectedEntity == entity)
				m_SelectedEntity = {};
		}
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

	/* 按字段元数据自动生成控件（schema 驱动） */
	static void DrawComponentFieldsBySchema(const ComponentDesc& desc, void* data)
	{
		for (const FieldDesc& field : desc.Fields)
		{
			/* 条件不满足：不画该行（字段值保留，序列化照常） */
			if (!EvaluateCondition(field.Condition, data))
				continue;

			ImGui::PushID(field.Name);

			if (field.Get != nullptr && field.Set != nullptr)
			{
				/* 访问器字段：先读入缓冲，编辑后写回 */
				alignas(16) uint8_t buffer[kFieldValueCapacity] = {};
				field.Get(data, buffer);
				DrawFieldControl(field, buffer);
				field.Set(data, buffer);
			}
			else
			{
				DrawFieldControl(field, static_cast<uint8_t*>(data) + field.Offset);
			}

			ImGui::PopID();
		}
	}

	/* 绘制单个组件块（折叠标题 + schema 字段 + 自定义绘制 + 移除菜单） */
	static void DrawComponentBlock(const ComponentDesc& desc, Entity& entity, void* component)
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
			DrawComponentFieldsBySchema(desc, component);
			if (desc.CustomDraw != nullptr)
				desc.CustomDraw(component);
			ImGui::TreePop();
		}

		/* 删除组件 */
		if (remove && desc.Remove != nullptr)
			desc.Remove(entity);
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
						variant.Add(m_SelectedEntity);
						ImGui::CloseCurrentPopup();
					}
				}
			}

			ImGui::EndPopup();
		}
	}
}
