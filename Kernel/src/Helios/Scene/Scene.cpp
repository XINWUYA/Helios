#include "Pch.h"
#include "Scene.h"
#include <tinyxml2.h>
#include "Components.h"
#include "Entity.h"
#include "Material.h"
#include "Helios/Common/Utils.h"
#include "Camera.h"
#include "Helios/Renderer/Renderer.h"
#include "Helios/Renderer/RenderView.h"
#include "SceneCommon.h"

namespace Helios
{
	Scene::Scene()
	{
		m_pReflectionProbeManager = CreateSharedPtr<ReflectionProbeManager>();

		/* 统一连接所有组件类型的信号 */
        ConnectSignalsForComponents(SceneComponentList{});
	}

	Scene::~Scene()
	{
		m_RenderViews.clear();

		/* 先断开所有组件的信号连接 */
		DisconnectSignalsForComponents(SceneComponentList{});
		ClearAllEntities();
	}

	/* 创建一个实体 */
	Entity Scene::CreateEntity(const std::string& name)
	{
		PROFILE_FUNCTION();

		Entity entity = { m_Registry.create(), shared_from_this() };

		/* 默认添加名称组件和变换组件 */
		entity.AddComponent<NameComponent>(name.empty() ? "New Entity" : name);
		entity.AddComponent<TransformComponent>();

		return entity;
	}

	/* 销毁实体 */
	void Scene::DestroyEntity(Entity& entity)
	{
		PROFILE_FUNCTION();

		m_Registry.destroy(entity);
	}

	void Scene::OnUpdate(float delta_time, Camera* editor_camera)
	{
		PROFILE_FUNCTION();

		/* 统一将 TransformComponent 同步给所有 SceneObject（Model / Light / Camera）。
		 * 具体变换如何写入由各 SceneObject 重写的 SetTransform 决定（多态）。 */
		const auto transform_view = m_Registry.view<TransformComponent>();
		for (auto entity : transform_view)
		{
			const auto& transform_component = transform_view.get<TransformComponent>(entity);

			if (auto* model_component = m_Registry.try_get<ModelComponent>(entity); model_component && model_component->m_Model)
				model_component->m_Model->SetTransform(transform_component.GetTransform());

			if (auto* light_component = m_Registry.try_get<LightComponent>(entity); light_component && light_component->m_Light)
				light_component->m_Light->SetTransform(transform_component.GetTransform());

			if (auto* camera_component = m_Registry.try_get<CameraComponent>(entity); camera_component && camera_component->m_Camera)
				camera_component->m_Camera->SetTransform(transform_component.GetTransform());

			if (auto* probe_component = m_Registry.try_get<ReflectionProbeComponent>(entity); probe_component && probe_component->m_ReflectionProbe)
				probe_component->m_ReflectionProbe->SetTransform(transform_component.GetTransform());
		}

		/* 收集RenderView，并在收集前更新 Camera 的视图/投影矩阵 */
		m_RenderViews.clear();
		const auto& camera_entities = m_Registry.view<CameraComponent>();
		for (auto entity : camera_entities)
		{
			auto& camera_component = camera_entities.get<CameraComponent>(entity);
			camera_component.m_Camera->OnUpdate(delta_time);

			auto* render_view = camera_component.m_Camera->GetRenderView();
			render_view->SetOwnerScene(shared_from_this());
			m_RenderViews.emplace_back(render_view);
		}

		/* Editor Camera's RenderView, 最后一个是编辑器RenderView */
		if (editor_camera)
		{
			auto* render_view = editor_camera->GetRenderView();
			render_view->SetOwnerScene(shared_from_this());
			m_RenderViews.emplace_back(render_view);
		}

		/* 按优先级进行排序，优先级大的先画 todo：不需要每帧排序 */
		std::sort(m_RenderViews.begin(), m_RenderViews.end(), [](RenderView* lft, RenderView* rht) {
			if (!lft && !rht) return false;
			if (!lft) return false;
			if (!rht) return true;
			return lft->GetPriority() > rht->GetPriority();
			});
	}

	void Scene::Render()
	{
		Renderer::Update();

		/* 绘制所有View */
		for (const auto& view : m_RenderViews)
		{
			Renderer::RenderAView(view);
		}
	}

	Entity Scene::GetPrimaryCameraEntity()
	{
		PROFILE_FUNCTION();

		/* difference of view and group: https://github.com/skypjack/entt/discussions/638 */
		const auto entity_view = m_Registry.view<CameraComponent>();
		for (auto& entity : entity_view)
		{
			const auto& camera_component = entity_view.get<CameraComponent>(entity);
			if (camera_component.m_IsPrimary)
				return Entity{ entity, shared_from_this() };
		}
		return {};
	}

	void Scene::Serializer(const std::string& path)
	{
		PROFILE_FUNCTION();

		auto* doc = new tinyxml2::XMLDocument();
		doc->InsertEndChild(doc->NewDeclaration());
		tinyxml2::XMLElement* scene_root = doc->NewElement("Scene");
		doc->InsertEndChild(scene_root);

		tinyxml2::XMLElement* entities_root = scene_root->InsertNewChildElement("Entities");

		/* 遍历场景中实体，进行序列化 */
		m_Registry.each(
			[&](auto& entity_id)
			{
				Entity entity = { entity_id, shared_from_this() };
				if (!entity)
					return;

				SerializeEntity(entities_root, entity);
			}
		);

		/* 保存到文本 */
		doc->SaveFile(path.c_str());

		delete doc;
	}

	bool Scene::Deserializer(const std::string& path)
	{
		PROFILE_FUNCTION();

		auto* doc = new tinyxml2::XMLDocument();
		tinyxml2::XMLError error = doc->LoadFile(path.c_str());
		if (error != tinyxml2::XML_SUCCESS)
		{
			CORE_LOG_ERROR("Failed to deserializer scene file: {}.", path);
			return false;
		}

		tinyxml2::XMLElement* scene_root = doc->FirstChildElement("Scene");
		tinyxml2::XMLElement* entities_root = scene_root->FirstChildElement("Entities");

		for (tinyxml2::XMLElement* entity_root = entities_root->FirstChildElement(); entity_root; entity_root = entity_root->NextSiblingElement("Entity"))
		{
			if (auto* id_attr = entity_root->FindAttribute("ID"))
			{
				uint32_t entity_id = id_attr->UnsignedValue();

				/* Name */
				std::string name;
				if (auto* name_attr = entity_root->FindAttribute("Name"))
					name = name_attr->Value();

				if (!name.empty())
				{
					auto entity = CreateEntity(name);

					/* Transform */
					if (const auto* transform_root = entity_root->FirstChildElement("Transform"))
					{
						auto& transform_component = entity.GetComponent<TransformComponent>();
						transform_component.m_Position = ToVec3(transform_root->Attribute("Position"));
						transform_component.m_Rotation = ToVec3(transform_root->Attribute("Rotation"));
						transform_component.m_Scale = ToVec3(transform_root->Attribute("Scale"));
					}

					/* Sprite */
					if (const auto* sprite_root = entity_root->FirstChildElement("Sprite"))
					{
						auto& sprite_component = entity.AddComponent<SpriteComponent>();
						const std::string texture_path = sprite_root->Attribute("TexturePath");
						sprite_component.m_Texture = DeviceTexture::Create(texture_path);
						sprite_component.m_BaseColor = ToVec4(sprite_root->Attribute("BaseColor"));
						sprite_component.m_TilingFactor = sprite_root->FloatAttribute("TilingFactor");
					}

					/* Camera */
					if (const auto* camera_root = entity_root->FirstChildElement("Camera"))
					{
						auto& camera_component = entity.AddComponent<CameraComponent>();
						camera_component.m_IsPrimary = camera_root->BoolAttribute("IsPrimary");
						camera_component.m_IsFixedAspectRatio = camera_root->BoolAttribute("IsFixedAspectRatio");

						const auto projection_type = static_cast<CameraProjectionType>(camera_root->IntAttribute("ProjectionType"));
						camera_component.m_Camera->SetProjectionType(projection_type);
						switch (projection_type)
						{
						case CameraProjectionType::Perspective:
							camera_component.m_Camera->SetFov(camera_root->FloatAttribute("Fov"));
							camera_component.m_Camera->SetNearClip(camera_root->FloatAttribute("Near"));
							camera_component.m_Camera->SetFarClip(camera_root->FloatAttribute("Far"));
							break;
						case CameraProjectionType::Orthographic:
							camera_component.m_Camera->SetHeightSize(camera_root->FloatAttribute("HeightSize"));
							camera_component.m_Camera->SetNearClip(camera_root->FloatAttribute("Near"));
							camera_component.m_Camera->SetFarClip(camera_root->FloatAttribute("Far"));
							break;
						}
					}

					/* Model */
					if (auto* model_root = entity_root->FirstChildElement("Model"))
					{
						auto& model_component = entity.AddComponent<ModelComponent>();

						const std::string model_path = model_root->Attribute("ModelPath");

						if (!model_path.empty())
							model_component.m_Model = Model::Create(model_path);
						

						/* todo: 内建模型处理 */

					}

					/* Light */
					if (auto* light_root = entity_root->FirstChildElement("Light"))
					{
						auto& component = entity.AddComponent<LightComponent>();
						component.m_Type = static_cast<LightType>(light_root->IntAttribute("LightType"));

						component.m_Light = Light::Create(component.m_Type);
						component.m_Light->SetColor(ToVec4(light_root->Attribute("LightColor")));
						component.m_Light->SetIntensity(light_root->FloatAttribute("LightIntensity"));
						component.m_Light->SetIsCastShadow(light_root->BoolAttribute("IsCastShadow"));
					}
				}
			}	
		}

		delete doc;
		return true;
	}

	void Scene::SerializeEntity(tinyxml2::XMLElement* root_node, Entity& entity)
	{
		PROFILE_FUNCTION();

		tinyxml2::XMLElement* entity_root = root_node->InsertNewChildElement("Entity");

		/* ID */
		entity_root->SetAttribute("ID", (uint32_t)entity);

		/* Name */
		if (entity.HasComponent<NameComponent>())
		{
			const auto& component = entity.GetComponent<NameComponent>();
			entity_root->SetAttribute("Name", component.m_Name.c_str());
		}

		/* Transform */
		if (entity.HasComponent<TransformComponent>())
		{
			auto* transform_root = entity_root->InsertNewChildElement("Transform");
			const auto& component = entity.GetComponent<TransformComponent>();
			transform_root->SetAttribute("Position", ToString(component.m_Position).c_str());
			transform_root->SetAttribute("Rotation", ToString(component.m_Rotation).c_str());
			transform_root->SetAttribute("Scale", ToString(component.m_Scale).c_str());
		}

		/* Sprite */
		if (entity.HasComponent<SpriteComponent>())
		{
			auto* sprite_root = entity_root->InsertNewChildElement("Sprite");
			const auto& component = entity.GetComponent<SpriteComponent>();
			sprite_root->SetAttribute("TexturePath", RELATIVE_PATH(component.m_Texture->GetPath()).c_str());
			sprite_root->SetAttribute("BaseColor", ToString(component.m_BaseColor).c_str());
			sprite_root->SetAttribute("TilingFactor", component.m_TilingFactor);
		}

		/* Camera */
		if (entity.HasComponent<CameraComponent>())
		{
			auto* camera_root = entity_root->InsertNewChildElement("Camera");
			const auto& component = entity.GetComponent<CameraComponent>();

			camera_root->SetAttribute("IsPrimary", component.m_IsPrimary);
			camera_root->SetAttribute("IsFixedAspectRatio", component.m_IsFixedAspectRatio);

			const auto projection_type = component.m_Camera->GetProjectionType();
			camera_root->SetAttribute("ProjectionType", static_cast<int>(projection_type));

			switch (projection_type)
			{
			case CameraProjectionType::Perspective:
			{
				camera_root->SetAttribute("Fov", component.m_Camera->GetFov());
				camera_root->SetAttribute("Near", component.m_Camera->GetNearClip());
				camera_root->SetAttribute("Far", component.m_Camera->GetFarClip());
			}
			break;
			case CameraProjectionType::Orthographic:
			{
				camera_root->SetAttribute("HeightSize", component.m_Camera->GetHeightSize());
				camera_root->SetAttribute("Near", component.m_Camera->GetNearClip());
				camera_root->SetAttribute("Far", component.m_Camera->GetFarClip());
			}
			break;
			}
		}

		/* Model */
		if (entity.HasComponent<ModelComponent>())
		{
			auto* model_root = entity_root->InsertNewChildElement("Model");
			const auto& component = entity.GetComponent<ModelComponent>();

			model_root->SetAttribute("ModelPath", RELATIVE_PATH(component.m_Model->GetPath()).c_str());
		}

		/* Light */
		if (entity.HasComponent<LightComponent>())
		{
			auto* model_root = entity_root->InsertNewChildElement("Light");
			const auto& component = entity.GetComponent<LightComponent>();

			model_root->SetAttribute("LightType", static_cast<int>(component.m_Type));
			model_root->SetAttribute("LightColor", ToString(component.m_Light->GetColor()).c_str());
			model_root->SetAttribute("LightIntensity", component.m_Light->GetIntensity());
			model_root->SetAttribute("IsCastShadow", component.m_Light->IsCastShadow());
		}
	}

    /* 对各组件类型连接on_construct/on_destroy信号 */
    template<typename... T>
    void Scene::ConnectSignalsForComponents(std::tuple<T...>)
    {
        (
            (m_Registry.on_construct<T>().template connect<&Scene::OnConstructComponent<T>>(this), ...),
            (m_Registry.on_destroy<T>().template connect<&Scene::OnDestroyComponent<T>>(this), ...)
            );
    }

    /* 对各组件类型断开on_construct/on_destroy信号（用于Scene析构阶段） */
    template<typename... T>
    void Scene::DisconnectSignalsForComponents(std::tuple<T...>)
    {
        (
            (m_Registry.on_construct<T>().disconnect(this), ...),
            (m_Registry.on_destroy<T>().disconnect(this), ...)
            );
    }

	/* on_construct关联组件的OnAdded */
	template<typename T>
	void Scene::OnConstructComponent(entt::registry&, entt::entity entity)
	{
		auto& component = m_Registry.get<T>(entity);
		if constexpr (std::is_base_of_v<ComponentBase, T>)
		{
			Entity e{ entity, shared_from_this() };
			component.OnAdded(*this, e);
		}
	}

	/* on_destroy关联组件的OnRemoved */
	template<typename T>
	void Scene::OnDestroyComponent(entt::registry&, entt::entity entity)
	{
		auto& component = m_Registry.get<T>(entity);
		if constexpr (std::is_base_of_v<ComponentBase, T>)
		{
			Entity e{ entity, shared_from_this() };
			component.OnRemoved(*this, e);
		}
	}
}
