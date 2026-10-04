#pragma once
#include <entt/entt.hpp>

namespace tinyxml2
{
	class XMLElement;
}

namespace Helios
{
	class Entity;
	class RenderView;
	class DeviceTexture;
	class Camera;
	class ReflectionProbeManager;

	/* 场景类 */
	class Scene final : public std::enable_shared_from_this<Scene>
	{
	public:
		Scene();
		~Scene();

		/* 创建一个实体 */
		Entity CreateEntity(const std::string& name = std::string());
		/* 销毁实体 */
		void DestroyEntity(Entity& entity);
		/* 清空所有实体，以及由实体派生的场景侧状态（反射探针管理器） */
		void ClearAllEntities();
		/* 销毁指定类型的实体 */
		template <typename T>
		void DestroyTargetEntities() { m_Registry.clear<T>(); }

		/* 获取场景的Registry */
		entt::registry& GetRegistry() { return m_Registry; }
		const entt::registry& GetRegistry() const { return m_Registry; }

		/* 更新相机 */
		void OnUpdate(float delta_time, Camera* editor_camera = nullptr);

		/* 渲染所有相机 */
		void Render();

		/* 获取主相机实体 */
		Entity GetPrimaryCameraEntity();

		/* 获取反射探针管理器（统一管理需要烘焙的反射探针） */
		const SharedPtr<ReflectionProbeManager>& GetReflectionProbeManager() const { return m_pReflectionProbeManager; }

		/* 序列化场景 */
		void Serializer(const std::string& path);
		bool Deserializer(const std::string& path);

	private:
		/* 对类型列表中的每个组件类型连接信号（由ConnectSignalsForComponents驱动展开） */
		template<typename... Ts>
        void ConnectSignalsForComponents(std::tuple<Ts...>);
        /* 断开所有组件的信号连接 */
        template<typename... Ts>
        void DisconnectSignalsForComponents(std::tuple<Ts...>);
		/* on_construct关联组件的OnAdded */
		template<typename T>
		void OnConstructComponent(entt::registry& registry, entt::entity entity);
        /* on_destroy关联组件的OnRemoved */
        template<typename T>
		void OnDestroyComponent(entt::registry& registry, entt::entity entity);
		/* 序列化一个实体 */
		void SerializeEntity(tinyxml2::XMLElement* root_node, Entity& entity);

		/* 场景中的所有实体都将注册到这里 */
		entt::registry m_Registry;

		/* RenderView列表 */
		std::vector<RenderView*> m_RenderViews;

        /* 反射探针管理器，统一管理需要烘焙的反射探针 */
        SharedPtr<ReflectionProbeManager> m_pReflectionProbeManager;
    };
}