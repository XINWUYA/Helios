#pragma once
#include <cstdint>
#include <functional>
#include <imgui.h>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "SceneInspector.h"
#include "UI/EditorIcons.h"
#include "Command/CommandStack.h"
#include "Helios/Scene/Entity.h"

namespace Helios
{
	class Scene;
	struct EntityTemplateDesc;

	/* 场景实体层级树，并持有属性面板（SceneInspector）。层级树按父子关系展开；层级面板 = 顶栏
	 * （搜索 + 筛选 + 新建）+ 实体树（子窗口）+ 底栏（计数），版式跟资源浏览器同源。行右端是可见性
	 * 开关（眼睛）。选中实体只在这里记一份，每帧交给属性面板去画。 */
	class SceneHierarchy
	{
	public:
		SceneHierarchy();
		SceneHierarchy(const SharedPtr<Scene>& scene);
		~SceneHierarchy() = default;

		/* 所属场景 */
		void SetOwnerScene(const SharedPtr<Scene>& scene);
		/* 当前场景的文件路径（空 = 从未保存过）：层级面板的根节点显示它的文件名。
		 * 由所属 Layer 在「改路径」的唯一入口里同步（见 SceneEditorLayer::SetActiveScenePath）。
		 * 展开状态按场景分开记，换场景时从这里切。 */
		void SetScenePath(const std::string& path);
		/* 树的展开状态存哪（按场景分段的小文件）：由所属 Layer 注入，空 = 只记在内存里。 */
		void SetTreeStatePath(const std::string& path);
		/* 场景有无未保存的改动：根节点名字后面据此显示脏标记。
		 * 由所属 Layer 每帧推送（见 SceneEditorLayer::OnImGuiRender），不缓存副本以免脱节。 */
		void SetSceneDirty(bool dirty) { m_SceneDirty = dirty; }
		/* 编辑历史：层级树与属性面板的改动都经它落地（未设置时直接改数据） */
		void SetCommandStack(CommandStack* command_stack)
		{
			m_pCommandStack = command_stack;
			m_Inspector.SetCommandStack(command_stack);
		}
		/* 绘制层级树，再画属性面板 */
		void OnImGuiRender();
		/* 选中实体：点实体同时把属性面板切回组件视图 */
		const Entity& GetSelectedEntity() const { return m_SelectedEntity; }
		void SetSelectedEntity(const Entity& entity);

		/* 双击行定位：把视口镜头带到这个实体上（跨面板能力，由所属 Layer 接线注入）。
		 * 未注入时双击只是选中，面板不查 Layer。 */
		using FocusEntityFunc = std::function<void(const Entity&)>;
		void SetFocusEntityFunc(FocusEntityFunc func) { m_FocusEntityFunc = std::move(func); }

		/* 属性面板：资源选中、跨面板入口、预览相机等只跟它有关的接口直接从这里取 */
		SceneInspector& GetInspector() { return m_Inspector; }
		const SceneInspector& GetInspector() const { return m_Inspector; }

		/* 最近一帧每条实体行的矩形（自检 / 自动化验证用，业务代码别依赖） */
		struct RowRect
		{
			Entity EntityHandle;
			ImVec2 Min;
			ImVec2 Max;
		};
		const std::vector<RowRect>& GetLastRowRects() const { return m_LastRowRects; }

	private:
		/* 父节点 -> 子节点。每帧由当前层级关系现算：父子关系只有 ParentComponent
		 * 一处数据源，这里不留第二份缓存，免得两边不一致。 */
		using EntityChildren = std::unordered_map<entt::entity, std::vector<Entity>>;

		/* 过滤后要显示的实体集合：名字命中的实体 + 它们的全部祖先。
		 * 祖先不能丢 —— 命中的子节点得有地方挂，否则树会散成一堆根节点。 */
		using FilterSet = std::unordered_set<entt::entity>;

		/* 类型筛选（顶栏右端的图标按钮 + 单选菜单）：与资源浏览器是同一套交互；
		 * All = 不按类型筛。实体的"类型"取它在树里那枚图标所属的类（见 EntityCategory）——
		 * 图标与筛选项永远是同一个问题的同一个答案。 */
		enum class TypeFilter : uint8_t
		{
			All = 0,
			Camera,
			Light,
			Model,
			Sprite,
			ReflectionProbe,
			Other,   /* 没有任何可识别组件的实体（树里用通用实体图标） */
			COUNT
		};

		/* 一次待落地的挂接请求：ImGui 遍历结束后才改层级 */
		struct PendingReparent
		{
			entt::entity Child{ entt::null };
			entt::entity Parent{ entt::null };
		};

		/* 展开状态表：键 = 实体在树里的名字路径哈希（见 HashTreeKey），值 = 是否展开。
		 * 用名字路径而不是实体句柄 —— 句柄会随增删实体漂移，名字路径跨场景、跨会话都对得上。 */
		using TreeOpenState = std::map<uint64_t, bool>;

		/* 一帧里画过的树行：底栏的「收起 / 展开全部」按它落笔 */
		struct TreeRowState
		{
			uint64_t Key{ 0 };
			bool     HasChildren{ false };
		};

		/* 显示场景实体列表UI */
		void ShowSceneHierarchyUI();
		/* 顶栏：左端搜索框，右端「类型筛选 + 新建实体」两枚图标按钮（跟资源浏览器同一套版式）。
		 * 放不下就压窄搜索框、右端不参与退让。筛选是纯图标按钮，点击弹类型单选菜单，生效时高亮。 */
		void ShowHierarchyTopBar();
		/* 底栏：实体计数贴行右端（自绘分隔线 + 垂直居中，跟资源浏览器的统计数同一套）。计数含为
		 * 挂住命中项留下的祖先 —— 就是屏幕上真能数出来的行数。 */
		void ShowHierarchyFooter(int shown_count, int total_count);
		/* 顶栏「新建」按钮的下拉菜单（画在面板根作用域，条目来自实体预设注册表） */
		void DrawNewEntityMenu();
		/* 顶栏「筛选」按钮的下拉菜单（画在面板根作用域）：类型单选列表，
		 * 行点击不收起弹层（连点切换档位，与工具栏的菜单同款） */
		void DrawTypeFilterMenu();
		/* 类型筛选下拉的显示名 */
		static const char* TypeFilterName(TypeFilter filter);
		/* 类型筛选菜单行的图标：与树里各类型用的图标同一套（All = 漏斗本体） */
		static Icons::Id FilterIconOf(TypeFilter filter);
		/* 实体归到哪一类：取它在树里那枚图标所属的类（图标与筛选项不会各说各话） */
		static TypeFilter EntityCategory(const Entity& entity);
		/* 面板是否处于"有筛选"状态（名字或类型任一生效）—— 收集与绘制两处共用同一判据 */
		[[nodiscard]] bool HasActiveFilter() const
		{
			return m_EntityFilter[0] != '\0' || m_TypeFilter != TypeFilter::All;
		}
		/* 过滤词 / 类型筛选命中的实体 + 它们的全部祖先（父链不能断） */
		void CollectFilteredEntities(FilterSet& out) const;
		/* 显示场景根节点（当前场景名）及其下的实体树 */
		void ShowSceneRootNode(const std::vector<Entity>& roots, const EntityChildren& children,
		                       Entity& pending_delete, const FilterSet* filter);
		/* 「新建实体」菜单项（条目来自实体预设注册表） */
		void ShowCreateEntityMenu();
		/* 收集根节点与 父->子 列表（按句柄升序，顺序稳定） */
		void BuildHierarchy(EntityChildren& children, std::vector<Entity>& roots) const;
		/* 显示实体节点及其子树；pending_delete 累积删除请求，由调用方在遍历后执行。
		 * parent_key 是父节点在展开状态表里的键（树根用 kTreeRootKey）；parent_hidden = 祖先里有被隐藏的。 */
		void ShowEntityNode(const Entity& entity, const EntityChildren& children,
		                    Entity& pending_delete, const FilterSet* filter, uint64_t parent_key,
		                    bool parent_hidden);
		/* 底栏「收起 / 展开全部」：把本帧画过的、有子节点的行统一置成 open */
		void SetAllNodesOpen(bool open);
		/* 本帧有没有能收起的行（全是没有子节点的实体时，底栏那枚按钮置灰） */
		[[nodiscard]] bool HasExpandableBranch() const;
		/* 换场景时把该场景的展开记录取出来（指向状态表里的那一份，引用稳定） */
		void BindTreeState();
		/* 展开记录读盘 / 落盘（整份读写，文件很小；没注入路径时不碰磁盘） */
		void LoadTreeStates();
		void StoreTreeStates();
		/* 应用本帧请求的挂接 */
		void ApplyPendingReparent();
		/* 拖到面板之外松手：默认挂到场景下（成为场景的一级节点） */
		void ApplyDropOutsidePanel();
		/* 改变父节点（保持世界变换）；有命令栈时可撤销 */
		void ReparentEntity(Entity entity, entt::entity parent);
		/* 按预设新建实体；有命令栈时实体本身也进编辑历史 */
		Entity CreateEntityFromTemplate(const EntityTemplateDesc& template_desc);
		/* 删除实体；有命令栈时可撤销 */
		void DeleteEntity(Entity entity);

		/* 所属场景 */
		SharedPtr<Scene> m_pOwnerScene;
		/* 当前场景文件路径（空 = 从未保存过） */
		std::string m_ScenePath;
		/* 展开状态文件（由 Layer 注入；空 = 只记在内存里，自检程序就是这样） */
		std::string m_TreeStatePath;
		/* 全部场景的展开记录：场景路径 -> 状态表（按场景分段存同一个文件） */
		std::map<std::string, TreeOpenState> m_TreeStates;
		/* 磁盘那份是否已经读过（首次用到时才读） */
		bool m_TreeStatesLoaded{ false };
		/* 当前场景的那一份（指向 m_TreeStates 里的条目，std::map 的引用稳定） */
		TreeOpenState* m_pTreeState{ nullptr };
		/* 本帧画过的树行（底栏按钮据此收起 / 展开） */
		std::vector<TreeRowState> m_TreeRows;
		/* 本帧有没有展开着的分支 —— 底栏按钮的图标与动作都由它决定 */
		bool m_HasOpenBranch{ false };
		/* 展开记录有改动，帧末落盘 */
		bool m_TreeStateDirty{ false };
		/* 场景有未保存的改动（由所属 Layer 每帧推送） */
		bool m_SceneDirty{ false };
		/* 选中实体（层级树与属性面板共用这一份） */
		Entity m_SelectedEntity;
		/* 双击行定位（由所属 Layer 注入；未注入时双击无动作） */
		FocusEntityFunc m_FocusEntityFunc;
		/* 本帧画过的实体行矩形（见 GetLastRowRects） */
		std::vector<RowRect> m_LastRowRects;
		/* 属性面板 */
		SceneInspector m_Inspector;
		/* 本帧请求的挂接（拖拽） */
		PendingReparent m_PendingReparent;
		/* 编辑历史（由所属 Layer 注入） */
		CommandStack* m_pCommandStack{ nullptr };
		/* 顶栏「新建实体」菜单的过滤词（与添加组件菜单同款：打开即清空并聚焦） */
		char m_NewEntityFilter[64]{};
		/* 层级面板的过滤词（空 = 不过滤） */
		char m_EntityFilter[64]{};
		/* 类型筛选（顶栏右端的图标按钮 + 单选菜单） */
		TypeFilter m_TypeFilter{ TypeFilter::All };
		/* 顶栏「新建」菜单的弹层 ID：在面板根作用域上算一次，
		 * 按钮（开）与菜单（画）两边按同一个 ID 对接 */
		ImGuiID m_PopupNewEntity{ 0 };
		/* 顶栏「筛选」菜单的弹层 ID（同上）与弹层锚点（按钮右下角，
		 * 弹层锚在它正下方、右缘对齐——与工具栏的 Debug View / Gizmos 菜单同款锚法） */
		ImGuiID m_PopupTypeFilter{ 0 };
		ImVec2 m_PopupTypeFilterAnchor{ 0.0f, 0.0f };
	};
}

