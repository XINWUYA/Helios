#include "Pch.h"
#include "SceneHierarchy.h"
#include "EditorIcons.h"
#include "PanelRegistry.h"

namespace Helios
{
	SceneHierarchy::SceneHierarchy()
	{
		PROFILE_FUNCTION();

		InitIcons();
	}

	SceneHierarchy::SceneHierarchy(const SharedPtr<Scene>& scene)
	{
		PROFILE_FUNCTION();

		SetOwnerScene(scene);
		InitIcons();
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

			/* 空白处右键，唤出新建 */
			if (ImGui::BeginPopupContextWindow("New", 1, false))
			{
				if (ImGui::BeginMenu("New A Entity"))
				{
					if (ImGui::MenuItem("Empty"))
					{
						const auto entity = m_pOwnerScene->CreateEntity("Empty");
						m_SelectedEntity = entity;
					}

					if (ImGui::MenuItem("Sprite"))
					{
						auto entity = m_pOwnerScene->CreateEntity("New Sprite");
						entity.AddComponent<SpriteComponent>();
						m_SelectedEntity = entity;
					}

					if (ImGui::MenuItem("Camera"))
					{
						auto entity = m_pOwnerScene->CreateEntity("New Camera");
						entity.AddComponent<CameraComponent>();
						m_SelectedEntity = entity;
					}

					if (ImGui::MenuItem("Directional Light"))
					{
						auto entity = m_pOwnerScene->CreateEntity("New Directional Light");
						entity.AddComponent<LightComponent>(LightType::Directional);
						m_SelectedEntity = entity;
					}

					if (ImGui::MenuItem("Point Light"))
					{
						auto entity = m_pOwnerScene->CreateEntity("New Point Light");
						entity.AddComponent<LightComponent>(LightType::Point);
						m_SelectedEntity = entity;
					}

					if (ImGui::MenuItem("Spot Light"))
					{
						auto entity = m_pOwnerScene->CreateEntity("New Spot Light");
						entity.AddComponent<LightComponent>(LightType::Spot);
						m_SelectedEntity = entity;
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

	void SceneHierarchy::InitIcons()
	{
		PROFILE_FUNCTION();

		m_pAddComponentIcon = Icons::GetTexture(Icons::Id::Add);
		m_pMenuIcon = Icons::GetTexture(Icons::Id::Menu);
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

	/* 组件框架 */
	template<typename T, typename UIFunction>
	static void ShowComponent(const std::string& name, Entity& entity, UIFunction show_custom)
	{
		PROFILE_FUNCTION();

		const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowItemOverlap | ImGuiTreeNodeFlags_FramePadding;
		if (entity.HasComponent<T>())
		{
			auto& component = entity.GetComponent<T>();
			const float panel_width = ImGui::GetContentRegionAvail().x;

			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{ 4, 4 });
			//float lineHeight = GImGui->Font->FontSize + GImGui->Style.FramePadding.y * 2.0f;
			ImGui::Separator();
			bool open = ImGui::TreeNodeEx((void*)typeid(T).hash_code(), flags, name.c_str());
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
				show_custom(component);
				ImGui::TreePop();
			}

			/* 删除组件 */
			if (remove)
				entity.RemoveComponent<T>();
		}
	}

	void SceneHierarchy::ShowEntityComponents()
	{
		PROFILE_FUNCTION();

		/* 实体名称组件 */
		ShowNameComponent();

		/* 增加组件按钮 */
		const float panel_width = ImGui::GetContentRegionAvail().x; /* 窗口区域宽度 */
		ImGui::SameLine(panel_width - 15); /* 放置在同行靠右的位置 */
		ShowAddComponentButton();

		/* 空间变换组件 */
		ShowTransformComponent();

		/* 图片精灵组件 */
		ShowSpriteComponent();

		/* 场景相机组件 */
		ShowCameraComponent();

		/* 模型组件 */
		ShowModelComponent();

		/* 光源组件 */
		ShowLightComponent();
	}

	/* 实体名称组件 */
	void SceneHierarchy::ShowNameComponent()
	{
		PROFILE_FUNCTION();

		if (m_SelectedEntity.HasComponent<NameComponent>())
		{
			PROFILE_SCOPE("Show NameComponent");

			auto& name = m_SelectedEntity.GetComponent<NameComponent>().m_Name;

			char buffer[256] = {};
			std::strncpy(buffer, name.c_str(), sizeof(buffer));
			ImGui::Text("Name:");
			ImGui::SameLine();
			if (ImGui::InputText("##Name", buffer, sizeof(buffer)))
			{
				name = std::string(buffer);
			}
		}
	}

	/* 增加组件按钮 */
	void SceneHierarchy::ShowAddComponentButton()
	{
		PROFILE_FUNCTION();

		START_STYLE_ALPHA(0.5f);
		if (ImGui::ImageButton((ImTextureID)m_pAddComponentIcon.get(), ImVec2(20, 20), ImVec2(0, 1), ImVec2(1, 0)))
			ImGui::OpenPopup("AddComponentPopup");
		END_STYLE_ALPHA;

		if (ImGui::BeginPopup("AddComponentPopup"))
		{
			/* 相机 */
			if (ImGui::MenuItem("Camera Component"))
			{
				m_SelectedEntity.AddComponent<CameraComponent>();
				ImGui::CloseCurrentPopup();
			}

			/* 图片精灵 */
			if (ImGui::MenuItem("Sprite Component"))
			{
				m_SelectedEntity.AddComponent<SpriteComponent>();
				ImGui::CloseCurrentPopup();
			}

			/* 模型 */
			if (ImGui::MenuItem("Model Component"))
			{
				m_SelectedEntity.AddComponent<ModelComponent>();
				ImGui::CloseCurrentPopup();
			}

			/* 光源 */
			if (ImGui::MenuItem("Light Component"))
			{
				m_SelectedEntity.AddComponent<LightComponent>();
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
		}
	}

	/* 空间变换组件 */
	void SceneHierarchy::ShowTransformComponent()
	{
		PROFILE_FUNCTION();

		ShowComponent<TransformComponent>("Transform", m_SelectedEntity,
			[](auto& component)
			{
				ImGuiExt::DrawVec3ControlUI("Position", component.m_Position, 0.0f);
				glm::vec3 rotation = glm::degrees(component.m_Rotation);
				ImGuiExt::DrawVec3ControlUI("Rotation", rotation, 0.0f);
				component.m_Rotation = glm::radians(rotation);
				ImGuiExt::DrawVec3ControlUI("Scale", component.m_Scale, 1.0f);

			});
	}

	/* 图片精灵组件 */
	void SceneHierarchy::ShowSpriteComponent()
	{
		PROFILE_FUNCTION();

		ShowComponent<SpriteComponent>("Sprite", m_SelectedEntity,
			[](auto& component)
			{
				ImGuiExt::DrawColorUI("BaseColor", component.m_BaseColor);
				ImGuiExt::DrawTextureUI("Texture", component.m_Texture, component.m_TilingFactor);
			});
	}

/* 场景相机组件 */
void SceneHierarchy::ShowCameraComponent()
{
	PROFILE_FUNCTION();

	ShowComponent<CameraComponent>("Camera", m_SelectedEntity,
		[](auto& component)
		{
			ImGuiExt::DrawCheckboxUI("IsPrimary", component.m_IsPrimary);
			ImGuiExt::DrawCheckboxUI("IsFixedAspectRatio", component.m_IsFixedAspectRatio);
			auto& camera = component.m_Camera;
			int projection_idx = static_cast<int>(camera->GetProjectionType());
			ImGuiExt::DrawComboUI("ProjectionType", GetEnumNames<CameraProjectionType>(), projection_idx,
				[&camera](int selected_idx)
				{
					camera->SetProjectionType(static_cast<CameraProjectionType>(selected_idx));
				});

			switch (camera->GetProjectionType())
			{
			case CameraProjectionType::Perspective:
			{
				float fov = camera->GetFov();
				ImGuiExt::DrawDragFloatUI("Fov", fov);
				float near_clip = camera->GetNearClip();
				ImGuiExt::DrawDragFloatUI("Near", near_clip);
				float far_clip = camera->GetFarClip();
				ImGuiExt::DrawDragFloatUI("Far", far_clip);

				camera->SetFov(fov);
				camera->SetNearClip(near_clip);
				camera->SetFarClip(far_clip);
			}
			break;
			case CameraProjectionType::Orthographic:
			{
				float height_size = camera->GetHeightSize();
				ImGuiExt::DrawDragFloatUI("HeightSize", height_size);
				float near_clip = camera->GetNearClip();
				ImGuiExt::DrawDragFloatUI("Near", near_clip);
				float far_clip = camera->GetFarClip();
				ImGuiExt::DrawDragFloatUI("Far", far_clip);

				camera->SetHeightSize(height_size);
				camera->SetNearClip(near_clip);
				camera->SetFarClip(far_clip);
			}
			break;
			}
		});
}

	/* 模型组件 */
	void SceneHierarchy::ShowModelComponent()
	{
		PROFILE_FUNCTION();

		ShowComponent<ModelComponent>("Model", m_SelectedEntity,
			[](auto& component)
			{
				auto& model = component.m_Model;

				if (model)
				{
					ImGui::PushID("ModelPath");
					ImGui::Columns(2);

					/* Label */
					ImGui::SetColumnWidth(0, 100);
					ImGui::Text("ModelPath");

					/* Texture */
					ImGui::NextColumn();
					{
						ImGui::TextWrapped(model->GetPath().c_str());
					}

					ImGui::Columns(1);
					ImGui::PopID();
				}

				/*const auto& mesh_segments = model->GetMeshSegments();
				for (auto& mesh_segment : mesh_segments)
				{
					auto* mesh_segment_root = model_root->InsertNewChildElement("MeshSegment");
					mesh_segment_root->SetAttribute("Name", mesh_segment->GetName().c_str());
					auto* mtl_root = mesh_segment_root->InsertNewChildElement("Material");
					mtl_root->SetAttribute("ShaderPath", mesh_segment->GetMaterial()->GetShader()->GetPath().c_str());
				}*/
			});
	}

	/* 光源组件 */
	void SceneHierarchy::ShowLightComponent()
	{
		PROFILE_FUNCTION();
		ShowComponent<LightComponent>("Light", m_SelectedEntity,
			[](auto& component)
			{
				SharedPtr<Light>& light = component.m_Light;
				if (!light)
					return;

				/* 光源类型 */
				int type_idx = static_cast<int>(light->GetLightType());
				ImGuiExt::DrawComboUI("Type", GetEnumNames<LightType>(), type_idx,
					[&light](int selected_idx)
					{
						/* todo: 切换光源类型 */
						//scene_camera.SetProjectionType(static_cast<SceneCamera::ProjectionType>(selected_idx));
					});

				/* 光源颜色 */
				glm::vec4 light_color = light->GetColor();
				ImGuiExt::DrawColorUI("Color", light_color);
				light->SetColor(light_color);

				/* 光源强度 */
				float light_intensity = light->GetIntensity();
				ImGuiExt::DrawDragFloatUI("Intensity", light_intensity);
				light->SetIntensity(light_intensity);

				/* 投影 */
				bool cast_shadow = light->IsCastShadow();
				ImGuiExt::DrawCheckboxUI("CastShadow", cast_shadow);
				light->SetIsCastShadow(cast_shadow);
			});
	}
}
