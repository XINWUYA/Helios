#include "Pch.h"
#include "Scene.h"
#include <tinyxml2.h>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include "Components.h"
#include "Entity.h"
#include "Material.h"
#include "Helios/Common/Utils.h"
#include "Helios/Common/Math.h"
#include "Camera.h"
#include "Helios/Renderer/Renderer.h"
#include "Helios/Renderer/RenderView.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include "SceneCommon.h"

namespace Helios
{
	namespace
	{
		/* 父链深度上限：兜住畸形场景文件里可能出现的环（正常层级远小于此） */
		constexpr size_t kMaxHierarchyDepth = 64;
	}

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

		/* 子节点先脱离并保持世界位置：父实体的句柄马上会失效，
		 * 子节点若继续指向它就会从层级树里消失，空间位置也会跟着跳变。 */
		std::vector<entt::entity> children;
		const auto parent_view = m_Registry.view<ParentComponent>();
		for (const entt::entity candidate : parent_view)
		{
			if (parent_view.get<ParentComponent>(candidate).m_Parent == static_cast<entt::entity>(entity))
				children.emplace_back(candidate);
		}

		/* 提升层级会改写 ParentComponent（结构变更），必须与遍历分开做 */
		for (const entt::entity child : children)
			SetParent(child, entt::null);

		m_Registry.destroy(entity);
	}

	entt::entity Scene::GetParent(entt::entity entity) const
	{
		if (!m_Registry.valid(entity))
			return entt::null;

		const auto* parent_component = m_Registry.try_get<ParentComponent>(entity);
		if (parent_component == nullptr || !m_Registry.valid(parent_component->m_Parent))
			return entt::null;

		return parent_component->m_Parent;
	}

	bool Scene::IsAncestor(entt::entity ancestor, entt::entity entity) const
	{
		if (ancestor == entt::null)
			return false;

		/* 沿父链上溯；深度上限兜住畸形场景文件里可能出现的环 */
		entt::entity current = entity;
		for (size_t depth = 0; current != entt::null && depth < kMaxHierarchyDepth; ++depth)
		{
			if (current == ancestor)
				return true;

			current = GetParent(current);
		}
		return false;
	}

	glm::mat4 Scene::GetWorldTransform(entt::entity entity) const
	{
		/* 先自下而上收集父链，再自顶向下累积：一趟 O(深度)，不需要递归 */
		entt::entity chain[kMaxHierarchyDepth];
		size_t depth = 0;

		entt::entity current = entity;
		while (current != entt::null && depth < kMaxHierarchyDepth && m_Registry.valid(current))
		{
			chain[depth++] = current;
			current = GetParent(current);
		}

		glm::mat4 world{ 1.0f };
		while (depth > 0)
		{
			if (const auto* transform_component = m_Registry.try_get<TransformComponent>(chain[--depth]))
				world *= transform_component->GetTransform();
		}
		return world;
	}

	bool Scene::SetParent(entt::entity child, entt::entity parent)
	{
		if (!m_Registry.valid(child))
			return false;

		/* child 是 parent 自身或祖先：挂上去会成环（IsAncestor 含自身，覆盖了自挂） */
		if (IsAncestor(child, parent))
			return false;

		const glm::mat4 world = GetWorldTransform(child);
		const glm::mat4 parent_world = GetWorldTransform(parent);

		SetParentLink(child, parent);

		/* 换父空间后本地变换重算：世界变换保持不变，挂上去不会跳位置 */
		if (auto* transform_component = m_Registry.try_get<TransformComponent>(child))
		{
			TransformComponent reparented = *transform_component;
			if (DecomposeTransform(glm::inverse(parent_world) * world,
				reparented.m_Position, reparented.m_Rotation, reparented.m_Scale))
			{
				*transform_component = reparented;
			}
		}
		return true;
	}

	void Scene::SetParentLink(entt::entity child, entt::entity parent)
	{
		if (!m_Registry.valid(child))
			return;

		/* 回到根层级就不留组件：没有父节点的实体不该带着这条记录 */
		if (parent == entt::null || !m_Registry.valid(parent))
		{
			m_Registry.remove<ParentComponent>(child);
			return;
		}

		m_Registry.emplace_or_replace<ParentComponent>(child, parent);
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
		 * 传下去的是世界变换（沿父链累积），SceneObject 只认识世界空间。
		 * 具体变换如何写入由各 SceneObject 重写的 SetTransform 决定（多态）。 */
		const auto transform_view = m_Registry.view<TransformComponent>();
		for (auto entity : transform_view)
		{
			const glm::mat4 world_transform = GetWorldTransform(entity);

			if (auto* model_component = m_Registry.try_get<ModelComponent>(entity); model_component && model_component->m_Model)
				model_component->m_Model->SetTransform(world_transform);

			if (auto* light_component = m_Registry.try_get<LightComponent>(entity); light_component && light_component->m_Light)
				light_component->m_Light->SetTransform(world_transform);

			if (auto* camera_component = m_Registry.try_get<CameraComponent>(entity); camera_component && camera_component->m_Camera)
				camera_component->m_Camera->SetTransform(world_transform);

			if (auto* probe_component = m_Registry.try_get<ReflectionProbeComponent>(entity); probe_component && probe_component->m_ReflectionProbe)
				probe_component->m_ReflectionProbe->SetTransform(world_transform);
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

		/* 父子关系存的是父实体的句柄，加载后句柄会重新分配，
		 * 所以先建完所有实体并记下「文件里的 ID → 新实体」，再统一还原层级。 */
		std::vector<std::pair<const tinyxml2::XMLElement*, Entity>> loaded_entities;
		std::unordered_map<uint64_t, Entity> entities_by_file_id;

		for (tinyxml2::XMLElement* entity_root = entities_root->FirstChildElement(); entity_root; entity_root = entity_root->NextSiblingElement("Entity"))
		{
			const tinyxml2::XMLAttribute* id_attribute = entity_root->FindAttribute("ID");
			if (id_attribute == nullptr)
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

			entities_by_file_id.emplace(id_attribute->Unsigned64Value(), entity);
			loaded_entities.emplace_back(entity_root, entity);
		}

		/* 第二遍：还原父子关系。局部变换已经在第一遍按原样读入，
		 * 所以这里只补链接，不重算变换（否则世界位置会变）。 */
		for (const auto& [entity_root, entity] : loaded_entities)
		{
			const tinyxml2::XMLAttribute* parent_attribute = entity_root->FindAttribute("Parent");
			if (parent_attribute == nullptr)
				continue;

			const auto parent = entities_by_file_id.find(parent_attribute->Unsigned64Value());
			if (parent == entities_by_file_id.end())
				continue; /* 指向不存在的实体：按根节点处理 */

			/* 文件被改坏到出现环时直接断开，避免层级遍历无限上溯 */
			if (IsAncestor(static_cast<entt::entity>(entity), static_cast<entt::entity>(parent->second)))
				continue;

			SetParentLink(entity, static_cast<entt::entity>(parent->second));
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

		/* 父节点写在实体自己的属性上（同 ID / Name）：它是场景图结构，不是某个组件的字段。
		 * 存的是父实体的句柄，加载时按 ID 重新映射（见 Deserializer）。 */
		if (const entt::entity parent = GetParent(entity); parent != entt::null)
			entity_root->SetAttribute("Parent", static_cast<uint32_t>(parent));

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
