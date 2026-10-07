#pragma once
#include <entt/entt.hpp>
#include <glm/glm.hpp>

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

		/* ---- 实体层级 ----
		 * 父子关系存在子实体的 ParentComponent 里，Scene 是唯一读写入口。
		 * 所有涉及层级的写入都必须走这里，否则世界变换与层级会脱节。 */

		/* 实体是否还存在（句柄非空不代表实体还在） */
		[[nodiscard]] bool IsEntityValid(entt::entity entity) const { return m_Registry.valid(entity); }

		/* 父节点；没有父节点（或父节点已失效）时返回 entt::null */
		[[nodiscard]] entt::entity GetParent(entt::entity entity) const;

		/* ancestor 是否为 entity 的祖先（含 entity 自身），用于拒绝成环的挂接 */
		[[nodiscard]] bool IsAncestor(entt::entity ancestor, entt::entity entity) const;

		/* 实体是否参与渲染：自己和全部祖先都可见才算。「隐藏」= 挂 VisibilityComponent 且
		 * m_Visible=false（缺席 = 可见）；隐藏沿父链传染（父隐藏整棵子树不画，子节点自身状态不受
		 * 影响）；句柄无效返回 false。 */
		[[nodiscard]] bool IsEntityVisible(entt::entity entity) const;

		/* 世界变换 = 沿父链累积的局部变换；没有父节点时就是局部变换。
		 * 句柄无效时返回单位阵。 */
		[[nodiscard]] glm::mat4 GetWorldTransform(entt::entity entity) const;

		/* 把 child 挂到 parent 下（parent 为 entt::null 表示回到根层级）。
		 * 保持世界变换不变：局部变换按新的父空间重算（矩阵不可分解时保留原本地变换）。
		 * child 是 parent 自身或祖先（会成环）、child 已不存在时不做改动并返回 false。 */
		bool SetParent(entt::entity child, entt::entity parent);

		/* 只改父子链接、不动局部变换：反序列化与命令重放使用（变换由调用方负责） */
		void SetParentLink(entt::entity child, entt::entity parent);

		/* 获取场景的Registry */
		entt::registry& GetRegistry() { return m_Registry; }
		const entt::registry& GetRegistry() const { return m_Registry; }

		/* 每帧更新：同步实体变换、收集本帧要渲染的视图（场景相机 + 已登记的外部相机） */
		void OnUpdate(float delta_time);

		/* 外部相机登记 / 摘除。外部相机 = 不属于场景、但要渲染本场景的相机
		 * （编辑器视口相机、工具预览相机等）。登记后每帧与场景相机一起收集渲染
		 * （场景相机在前）；重复登记无效果。生命周期由调用方保证：相机销毁前先摘除。 */
		void AddExternalCamera(Camera* camera);
		void RemoveExternalCamera(Camera* camera);

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
		/* 外部相机（编辑器视口 / 工具预览等，登记式；见 AddExternalCamera） */
		std::vector<Camera*> m_ExternalCameras;

        /* 反射探针管理器，统一管理需要烘焙的反射探针 */
        SharedPtr<ReflectionProbeManager> m_pReflectionProbeManager;
    };
}