#include "Pch.h"
#include "Scene.h"
#include <tinyxml2.h>
#include <algorithm>
#include <vector>
#include "Components.h"
#include "Entity.h"
#include "Material.h"
#include "Helios/Common/Utils.h"
#include "Camera.h"
#include "Helios/Renderer/Renderer.h"
#include "Helios/Renderer/RenderView.h"
#include "Helios/Reflection/ComponentRegistry.h"
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

	void Scene::ClearAllEntities()
	{
		PROFILE_FUNCTION();

		/* 实体销毁会触发组件的 OnRemoved（反射探针随之从管理器注销），
		 * 但管理器仍可能持有引用，一并清空以保证不留上一份场景的残留。 */
		m_Registry.clear();

		if (m_pReflectionProbeManager != nullptr)
			m_pReflectionProbeManager->ClearProbes();
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

		/* 保存场景时才持久化烘焙结果：烘焙结果是显存中的中间状态，只有场景落盘时才有必要写文件。
		 * 这里只登记请求（写入要等 GPU 执行完，由探针的写入状态机在后续帧完成），
		 * 但缓存路径必须在序列化组件之前定下来，才能一并写进 .scn。 */
		if (m_pReflectionProbeManager != nullptr)
			m_pReflectionProbeManager->RequestBakeCacheWrites();

		/* 按实体句柄升序输出：registry 的遍历顺序与创建顺序无关，
		 * 直接遍历会让每次「加载 → 保存」把实体顺序整体翻转。 */
		std::vector<entt::entity> entities;
		m_Registry.each([&entities](auto entity_id) { entities.emplace_back(entity_id); });
		std::sort(entities.begin(), entities.end(),
			[](entt::entity lhs, entt::entity rhs)
			{
				return entt::to_integral(lhs) < entt::to_integral(rhs);
			});

		for (const auto entity_id : entities)
		{
			Entity entity = { entity_id, shared_from_this() };
			if (!entity)
				continue;

			SerializeEntity(entities_root, entity);
		}

		/* 保存到文本 */
		doc->SaveFile(path.c_str());

		delete doc;
	}

	bool Scene::Deserializer(const std::string& path)
	{
		PROFILE_FUNCTION();

		tinyxml2::XMLDocument doc;
		if (doc.LoadFile(path.c_str()) != tinyxml2::XML_SUCCESS)
		{
			CORE_LOG_ERROR("Failed to deserializer scene file: {}.", path);
			return false;
		}

		tinyxml2::XMLElement* scene_root = doc.FirstChildElement("Scene");
		tinyxml2::XMLElement* entities_root = (scene_root != nullptr) ? scene_root->FirstChildElement("Entities") : nullptr;
		if (entities_root == nullptr)
		{
			CORE_LOG_ERROR("Scene file has no <Scene>/<Entities>: {}.", path);
			return false;
		}

		/* 加载是「替换」语义：不清空会把上一份场景的实体留在新场景里。
		 * 先确认文件可加载再清空，加载失败时当前内容不受影响。 */
		ClearAllEntities();

		for (tinyxml2::XMLElement* entity_root = entities_root->FirstChildElement(); entity_root; entity_root = entity_root->NextSiblingElement("Entity"))
		{
			if (entity_root->FindAttribute("ID") == nullptr)
				continue;

			const char* name = entity_root->Attribute("Name");
			if (name == nullptr || *name == '\0')
				continue;

			Entity entity = CreateEntity(name);

			/* Name / Transform 由 CreateEntity 带上，其余组件由注册表驱动 */
			for (const ComponentDesc& desc : ComponentRegistry::Instance().All())
			{
				if (!desc.bSerializable || desc.Has == nullptr || desc.GetPtr == nullptr || desc.Add == nullptr)
					continue;

				const tinyxml2::XMLElement* element = entity_root->FirstChildElement(desc.Name);
				if (element == nullptr)
					continue;

				if (!desc.Has(entity))
					desc.Add(entity);

				void* component = desc.GetPtr(entity);
				if (component != nullptr)
					LoadComponentFromXml(desc, component, element);
			}
		}

		return true;
	}

	void Scene::SerializeEntity(tinyxml2::XMLElement* root_node, Entity& entity)
	{
		PROFILE_FUNCTION();

		tinyxml2::XMLElement* entity_root = root_node->InsertNewChildElement("Entity");

		/* ID */
		entity_root->SetAttribute("ID", (uint32_t)entity);

		/* Name 由实体自身的属性承载 */
		if (entity.HasComponent<NameComponent>())
			entity_root->SetAttribute("Name", entity.GetComponent<NameComponent>().m_Name.c_str());

		/* 其余组件由注册表驱动，元素名即组件名 */
		for (const ComponentDesc& desc : ComponentRegistry::Instance().All())
		{
			if (!desc.bSerializable || desc.Has == nullptr || desc.GetPtr == nullptr)
				continue;

			if (!desc.Has(entity))
				continue;

			void* component = desc.GetPtr(entity);
			if (component == nullptr)
				continue;

			SaveComponentToXml(desc, component, entity_root->InsertNewChildElement(desc.Name));
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
