#pragma once

namespace Helios
{
	class Scene;
	class Entity;

	/* 场景实体列表 + 属性面板
	 * 属性面板的组件 UI 由 ComponentRegistry 驱动，本类不依赖任何具体组件类型；
	 * 新增组件只需在注册表补一段（见 Helios/Reflection/ComponentRegistry.cpp）。 */
	class SceneHierarchy
	{
	public:
		SceneHierarchy();
		SceneHierarchy(const SharedPtr<Scene>& scene);
		~SceneHierarchy() = default;

		/* 所属场景 */
		void SetOwnerScene(const SharedPtr<Scene>& scene);
		/* 绘制相关UI */
		void OnImGuiRender();
		/* 选中实体 */
		const Entity& GetSelectedEntity() const { return m_SelectedEntity; }
		void SetSelectedEntity(const Entity& entity);

	private:
		/* 显示场景实体列表UI */
		void ShowSceneHierarchyUI();
		/* 显示选中实体属性 */
		void ShowEntityPropertiesUI();
		/* 显示实体节点（SceneHierarchy中的节点）*/
		void ShowEntityNode(Entity& entity);
		/* 显示选中实体的全部组件（遍历 ComponentRegistry，不认识具体类型） */
		void ShowEntityComponents();
		/* 增加组件按钮（菜单项同样来自注册表） */
		void ShowAddComponentButton();

		/* 所属场景 */
		SharedPtr<Scene> m_pOwnerScene;
		/* 选中实体 */
		Entity m_SelectedEntity;
	};
}
