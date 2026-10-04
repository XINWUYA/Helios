#pragma once
#include "Helios/Command/CommandStack.h"
#include "Helios/Reflection/ComponentRegistry.h"

namespace Helios
{
	class Scene;
	class Entity;
	struct EntityTemplateDesc;

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
		/* 编辑历史：字段改动经命令栈落地（未设置时直接改数据） */
		void SetCommandStack(CommandStack* command_stack) { m_pCommandStack = command_stack; }
		/* 绘制相关UI */
		void OnImGuiRender();
		/* 选中实体 */
		const Entity& GetSelectedEntity() const { return m_SelectedEntity; }
		void SetSelectedEntity(const Entity& entity);

	private:
		/* 显示场景实体列表UI */
		void ShowSceneHierarchyUI();
		/* 显示实体节点；返回 true 表示请求删除该实体 */
		bool ShowEntityNode(Entity& entity);
		/* 显示选中实体属性 */
		void ShowEntityPropertiesUI();
		/* 显示选中实体的全部组件（遍历 ComponentRegistry，不认识具体类型） */
		void ShowEntityComponents();
		/* 增加组件按钮（菜单项同样来自注册表） */
		void ShowAddComponentButton();

		/* 按预设新建实体；有命令栈时实体本身也进编辑历史 */
		Entity CreateEntityFromTemplate(const EntityTemplateDesc& template_desc);
		/* 删除实体；有命令栈时可撤销 */
		void DeleteEntity(Entity entity);

		/* 按字段元数据生成控件；编辑前后各取一次值，有变化则生成字段改动命令 */
		void DrawComponentFieldsBySchema(const ComponentDesc& desc, Entity& entity, void* component);
		/* 绘制单个组件块（折叠标题 + 字段 + 自定义绘制 + 移除菜单） */
		void DrawComponentBlock(const ComponentDesc& desc, Entity& entity, void* component);

		/* 所属场景 */
		SharedPtr<Scene> m_pOwnerScene;
		/* 选中实体 */
		Entity m_SelectedEntity;
		/* 编辑历史（由所属 Layer 注入） */
		CommandStack* m_pCommandStack{ nullptr };
		/* 字段编辑的合并窗口是否已打开（连续拖动合并为一条历史） */
		bool m_FieldEditTransactionOpen{ false };
	};
}
