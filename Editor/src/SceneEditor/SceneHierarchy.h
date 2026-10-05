#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "Helios/Command/CommandStack.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include "Helios/Scene/Entity.h"

namespace Helios
{
	class Scene;
	struct EntityTemplateDesc;

	/* 场景实体层级树 + 属性面板。层级树按父子关系展开；层级面板 = 顶栏（搜索 + 筛选 + 新建）+
	 * 实体树（子窗口）+ 底栏（计数），版式跟资源浏览器同源。行右端是可见性开关（眼睛）。
	 * 属性面板的组件 UI 由 ComponentRegistry 驱动，新增组件只需在注册表补一段。 */
	class SceneHierarchy
	{
	public:
		SceneHierarchy();
		SceneHierarchy(const SharedPtr<Scene>& scene);
		~SceneHierarchy() = default;

		/* 所属场景 */
		void SetOwnerScene(const SharedPtr<Scene>& scene);
		/* 当前场景的文件路径（空 = 从未保存过）：层级面板的根节点显示它的文件名。
		 * 由所属 Layer 在「改路径」的唯一入口里同步（见 SceneEditorLayer::SetActiveScenePath）。 */
		void SetScenePath(const std::string& path) { m_ScenePath = path; }
		/* 场景有无未保存的改动：根节点名字后面据此显示脏标记。
		 * 由所属 Layer 每帧推送（见 SceneEditorLayer::OnImGuiRender），不缓存副本以免脱节。 */
		void SetSceneDirty(bool dirty) { m_SceneDirty = dirty; }
		/* 编辑历史：字段改动经命令栈落地（未设置时直接改数据） */
		void SetCommandStack(CommandStack* command_stack) { m_pCommandStack = command_stack; }
		/* 绘制相关UI */
		void OnImGuiRender();
		/* 选中实体 */
		const Entity& GetSelectedEntity() const { return m_SelectedEntity; }
		void SetSelectedEntity(const Entity& entity);

	private:
		/* 父节点 -> 子节点。每帧由当前层级关系现算：父子关系只有 ParentComponent
		 * 一处数据源，这里不留第二份缓存，免得两边不一致。 */
		using EntityChildren = std::unordered_map<entt::entity, std::vector<Entity>>;

		/* 过滤后要显示的实体集合：名字命中的实体 + 它们的全部祖先。
		 * 祖先不能丢 —— 命中的子节点得有地方挂，否则树会散成一堆根节点。 */
		using FilterSet = std::unordered_set<entt::entity>;

		/* 字段控件的绘制回调：拿到字段元数据与可写的存储。
		 * 偏移字段拿到的是组件内的原位地址，访问器字段拿到的是栈上缓冲
		 * （回写由 DrawEditableField 负责），两者对回调是透明的。 */
		using FieldDrawFunc = void (*)(const FieldDesc& field, void* field_ptr);

		/* 一次待落地的挂接请求：ImGui 遍历结束后才改层级 */
		struct PendingReparent
		{
			entt::entity Child{ entt::null };
			entt::entity Parent{ entt::null };
		};

		/* 显示场景实体列表UI */
		void ShowSceneHierarchyUI();
		/* 层级面板头部：过滤框 + 实体计数（面板标题已在 Tab 上，不再重复场景名） */
		void ShowHierarchyHeader(int shown_count, int total_count);
		/* 过滤词命中的实体 + 它们的全部祖先（父链不能断） */
		void CollectFilteredEntities(FilterSet& out) const;
		/* 显示场景根节点（当前场景名）及其下的实体树 */
		void ShowSceneRootNode(const std::vector<Entity>& roots, const EntityChildren& children,
		                       Entity& pending_delete, const FilterSet* filter);
		/* 「新建实体」菜单项（条目来自实体预设注册表） */
		void ShowCreateEntityMenu();
		/* 收集根节点与 父->子 列表（按句柄升序，顺序稳定） */
		void BuildHierarchy(EntityChildren& children, std::vector<Entity>& roots) const;
		/* 显示实体节点及其子树；pending_delete 累积删除请求，由调用方在遍历后执行 */
		void ShowEntityNode(const Entity& entity, const EntityChildren& children,
		                    Entity& pending_delete, const FilterSet* filter);
		/* 应用本帧请求的挂接 */
		void ApplyPendingReparent();
		/* 拖到面板之外松手：默认挂到场景下（成为场景的一级节点） */
		void ApplyDropOutsidePanel();
		/* 改变父节点（保持世界变换）；有命令栈时可撤销 */
		void ReparentEntity(Entity entity, entt::entity parent);
		/* 显示选中实体属性 */
		void ShowEntityPropertiesUI();
		/* 面板头部：实体图标 + 名字 + 新增组件入口（让属性面板自带上下文） */
		void ShowPropertiesHeader();
		/* 显示选中实体的全部组件（遍历 ComponentRegistry，不认识具体类型） */
		void ShowEntityComponents();
		/* 无选中实体时的空状态提示 */
		void ShowEmptyPropertiesHint();
		/* 增加组件按钮（菜单项同样来自注册表） */
		void ShowAddComponentButton();

		/* 按预设新建实体；有命令栈时实体本身也进编辑历史 */
		Entity CreateEntityFromTemplate(const EntityTemplateDesc& template_desc);
		/* 删除实体；有命令栈时可撤销 */
		void DeleteEntity(Entity entity);

		/* 按字段元数据生成控件；编辑前后各取一次值，有变化则生成字段改动命令 */
		void DrawComponentFieldsBySchema(const ComponentDesc& desc, Entity& entity, void* component);
		/* 单个字段的编辑：取快照 → 交给 draw 画控件 → 比较 → 有变化则生成历史命令。
		 * 事务（连续拖动/输入合并成一条历史）也在这里开合，因此组件卡里的字段与
		 * 面板头部就地编辑的实体名共享同一套撤销语义。 */
		void DrawEditableField(const ComponentDesc& desc, Entity& entity, void* component,
		                       const FieldDesc& field, FieldDrawFunc draw);
		/* 绘制单个组件块：卡片 = 卡头（折叠箭头 + 组件图标 + 名称 + 右侧菜单）+ 卡身（字段） */
		void DrawComponentBlock(const ComponentDesc& desc, Entity& entity, void* component);

		/* 所属场景 */
		SharedPtr<Scene> m_pOwnerScene;
		/* 当前场景文件路径（空 = 从未保存过） */
		std::string m_ScenePath;
		/* 场景有未保存的改动（由所属 Layer 每帧推送） */
		bool m_SceneDirty{ false };
		/* 选中实体 */
		Entity m_SelectedEntity;
		/* 本帧请求的挂接（拖拽） */
		PendingReparent m_PendingReparent;
		/* 编辑历史（由所属 Layer 注入） */
		CommandStack* m_pCommandStack{ nullptr };
		/* 字段编辑的合并窗口是否已打开（连续拖动合并为一条历史） */
		bool m_FieldEditTransactionOpen{ false };
		/* 「添加组件」菜单的过滤词：组件一多，菜单需要能搜 */
		char m_AddComponentFilter[64]{};
		/* 层级面板的过滤词（空 = 不过滤） */
		char m_EntityFilter[64]{};
	};
}

