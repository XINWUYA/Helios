#pragma once
#include <filesystem>
#include <functional>
#include <imgui.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "CameraPreview.h"
#include "Command/AssetFileOps.h"
#include "Helios/Command/CommandStack.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include "Helios/Scene/Entity.h"

namespace Helios
{
	class Scene;
	class DeviceTexture;
	class Material;
	class MaterialGroup;
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
		/* 资源定位通道（跨面板能力）：材质卡里的贴图点击 → 资源浏览器切目录并选中。
		 * 由上层接线注入（装配层 → SceneEditorLayer → 这里），面板不查 Layer。 */
		using AssetRevealFunc = std::function<void(const std::string&)>;
		void SetAssetRevealFunc(AssetRevealFunc func) { m_AssetRevealFunc = std::move(func); }
		/* 绘制相关UI */
		void OnImGuiRender();
		/* 选中实体 */
		const Entity& GetSelectedEntity() const { return m_SelectedEntity; }
		void SetSelectedEntity(const Entity& entity);

		/* 资源选中（跨面板能力，由外壳接线：资源浏览器 → 这里）：
		 * 属性面板切换到该资源的详细内容；最近一次选择说了算 —— 点实体即切回组件视图
		 * （见 SetSelectedEntity）。传空（切目录 / Esc / 被删掉）则交回实体视图。 */
		void SetAssetSelection(const std::vector<AssetSelectionEntry>& selection);

		/* 相机预览是否在场景登记中（正在为属性面板渲染预览画面）。
		 * 面板每帧按「选中了相机 + 预览卡展开 + 面板窗口可见」重新挣取，见 ShowEntityPropertiesUI。 */
		[[nodiscard]] bool IsCameraPreviewActive() const { return m_IsCameraPreviewActive; }

	private:
		/* 父节点 -> 子节点。每帧由当前层级关系现算：父子关系只有 ParentComponent
		 * 一处数据源，这里不留第二份缓存，免得两边不一致。 */
		using EntityChildren = std::unordered_map<entt::entity, std::vector<Entity>>;

		/* 过滤后要显示的实体集合：名字命中的实体 + 它们的全部祖先。
		 * 祖先不能丢 —— 命中的子节点得有地方挂，否则树会散成一堆根节点。 */
		using FilterSet = std::unordered_set<entt::entity>;

		/* 类型筛选（底栏左端下拉）：与资源浏览器是同一套交互；All = 不按类型筛。
		 * 实体的"类型"取它在树里那枚图标所属的类（见 EntityCategory）——
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
		/* 层级里拖入 .mesh = 给实体挂 / 换模型：
		 * 无组件先补组件（AddComponentCommand）、再设模型（组件快照命令）——两步都可撤销。
		 * entity 按值（轻量句柄）：树遍历里拿到的是 const Entity&，拷贝一份再改。 */
		void ApplyDroppedMeshToEntity(Entity entity, const std::string& absolute_path);

		/* Model 的材质卡：每槽一张，排在 Model 组件卡之后（卡片背景走绘制通道、通道不可嵌套，
		 * "卡中卡"画不出来 —— 所以材质详情不能画在组件卡里，只能同级平铺）。 */
		void DrawModelMaterialCards(Entity entity, ModelComponent& component);
		/* 材质卡主体：当前材质的属性行（Shader + 参数，可就地编辑 → 自动实例化） */
		bool DrawMaterialCardBody(ModelComponent& component, const Model& model, int slot_index);
		/* Camera 的预览卡：被选中相机视野的实时画面（编辑器侧工具预览相机渲染，
		 * 见 CameraPreview.h）—— 与材质卡同一路数，平铺在组件卡之后（卡片不可嵌套）。 */
		void DrawCameraPreviewCard(Entity entity, CameraComponent& component);
		/* 同步一帧预览：首次请求时把预览相机登记为外部相机，此后每帧镜像来源相机参数 */
		void SyncCameraPreview(Entity entity, const SharedPtr<Camera>& camera, const glm::uvec2& render_size);
		/* 停止预览：摘除外部相机登记（预览画面不再参与场景渲染） */
		void StopCameraPreview();
		/* 组件自定义编辑的合并窗口封口（每帧调用；与 DrawEditableField 同一套事务规则） */
		void ServiceComponentEditTransaction();
		/* 顶栏：左端「新建实体」，右端搜索框（贴右端）。
		 * 版式与资源浏览器的顶栏同一套：控件贴上边、上下各留 1px、放不下时把搜索框压窄。
		 * （类型筛选不在这一栏：它搬到了底栏左端，见 ShowHierarchyFooter。） */
		void ShowHierarchyTopBar();
		/* 底栏：实体计数贴行右端（自绘分隔线 + 垂直居中，跟资源浏览器的统计数同一套）。计数含为
		 * 挂住命中项留下的祖先 —— 就是屏幕上真能数出来的行数。 */
		void ShowHierarchyFooter(const ImVec2& theme_padding, int shown_count, int total_count);
		/* 顶栏「新建」按钮的下拉菜单（画在面板根作用域，条目来自实体预设注册表） */
		void DrawNewEntityMenu();
		/* 类型筛选下拉的显示名 */
		static const char* TypeFilterName(TypeFilter filter);
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
		/* 显示选中的资源（头部 + 详情卡；多选时是摘要） */
		void ShowAssetProperties();
		/* 资源的详情卡：类型（卡头）/ 路径 / 大小 / 修改时间 + 类型相关的深挖行 + 图片预览 */
		void DrawAssetDetailsCard(const AssetSelectionEntry& entry);
		/* .mtl 的材质编辑卡：每条目一张，同级平铺在详情卡之后（与 probe 图卡同一路数）——
		 * Shader 可选可改、参数与光栅化状态可就地编辑；改动即时热更（同步缓存里的
		 * 共享材质 → 场景窗口立刻可见），「Apply」（没有改动时禁用）只负责序列化回文件 */
		void DrawAssetMaterialCards(const std::filesystem::path& absolute_path);
		/* 材质编辑卡的卡身：Shader 下拉（按目录分组的子菜单）+ 参数行 + 光栅化状态行；
		 * on_edit：任何改动走这里（调用方记脏 + 热更新） */
		void DrawAssetMaterialBody(Material& material, const std::function<void()>& on_edit);
		/* 「Apply」：保存修改并序列化到本地，并同步材质管理器里已缓存的共享实例 */
		void ApplyAssetMaterialEdits(const std::filesystem::path& absolute_path);
		/* 多选摘要卡：数量 / 合计大小 / 类型分布 */
		void DrawMultiAssetCard();
		/* 按 (路径, 写入时间) 缓存类型相关的深挖详情（图片尺寸 / 场景统计）——
		 * 要读文件内容的那种，只在选中项或文件变化时重算 */
		void RefreshAssetDetailRows(const AssetSelectionEntry& entry);
		/* 切某张烘焙图卡的预览 mip：重开缓存文件（数据不常驻）、按新层级重生成十字展开图 */
		void ApplyProbeCardMip(size_t card_index, int mip);
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
		/* 设置实体自身的可见性（层级行右端的眼睛开关）；有命令栈时可撤销。
		 * visible 为目标状态：隐藏 = 补 Visibility 组件并置 false，显示 = 去组件（缺席 = 可见）。 */
		void SetEntityVisibility(Entity entity, bool visible);

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
		/* 资源浏览器里选中的资源（由外壳推送；空 = 没有） */
		std::vector<AssetSelectionEntry> m_AssetSelection;
		/* 属性面板正显示资源详情（最近一次选择是资源） */
		bool m_AssetFocus{ false };
		/* 深挖详情的缓存键（路径 + 写入时间）与结果行 */
		std::string m_AssetDetailPath;
		std::filesystem::file_time_type m_AssetDetailWriteTime{};
		std::vector<std::pair<std::string, std::string>> m_AssetDetailRows;
		/* .probe 文件的烘焙结果图卡（跟深挖行用同一个缓存键刷新；同级平铺在详情卡后面）：每张烘焙图
		 * （环境 / 辐照度 / 预滤波）一张卡（名字 + 规格 + 十字展开预览）；多层带「Mip」切换。 */
		struct ProbeImageCard
		{
			std::string Label;                  /* 显示名（Environment / Irradiance / Prefilter） */
			std::string Spec;                   /* "512 x 512, RGBA16F, 10 mips" */
			std::string SourcePath;             /* .probe 绝对路径（切 mip 时重读；缓存数据不常驻） */
			std::string TextureName;            /* 设备纹理名（缓存文件名 + 标签） */
			int ImageIndex{ 0 };                /* 该图在缓存文件里的序号 */
			int Mip{ 0 };                       /* 当前预览的 mip 层级 */
			int MipCount{ 1 };                  /* 缓存里的 mip 层数（> 1 才画切换行） */
			uint32_t Mip0Size{ 0 };             /* mip0 边长（组合框标签用） */
			SharedPtr<DeviceTexture> Texture;   /* 十字展开预览；无图形上下文时为空 */
		};
		std::vector<ProbeImageCard> m_AssetProbeCards;
		/* 材质资产（.mtl）的编辑缓冲与可选 Shader：与深挖详情同一缓存键（路径 + 写入时间）
		 * 从磁盘读入；编辑只改缓冲（不碰场景与缓存），点「Apply」才序列化回文件。 */
		SharedPtr<MaterialGroup> m_AssetMaterialGroup;
		std::vector<std::string> m_AssetMaterialShaderOptions;
		/* 编辑缓冲有没有未保存的改动（Apply 按钮的禁用依据）：改 Shader / 参数时置位，
		 * 重读缓冲（换选中项 / 文件变化 / Apply 写盘后）清掉 */
		bool m_AssetMaterialDirty{ false };
		/* 本帧请求的挂接（拖拽） */
		PendingReparent m_PendingReparent;
		/* 编辑历史（由所属 Layer 注入） */
		CommandStack* m_pCommandStack{ nullptr };
		/* 资源定位（材质卡的贴图点击）；由上层注入，未注入时点击无动作 */
		AssetRevealFunc m_AssetRevealFunc;
		/* 相机预览：编辑器侧工具预览相机（懒创建 —— 没预览过相机的会话不付这份成本） */
		UniquePtr<CameraPreview> m_pCameraPreview{ nullptr };
		/* 预览相机已登记进场景（正在渲染预览画面） */
		bool m_IsCameraPreviewActive{ false };
		/* 本帧是否请求预览（帧首清零、预览卡置位、帧末结算起停；见 ShowEntityPropertiesUI） */
		bool m_CameraPreviewWanted{ false };
		/* 字段编辑的合并窗口是否已打开（连续拖动合并为一条历史） */
		bool m_FieldEditTransactionOpen{ false };
		/* 组件自定义绘制的合并窗口是否已打开（同上，走 CustomDraw 通道：材质参数拖动等） */
		bool m_ComponentEditTransactionOpen{ false };
		/* 「添加组件」菜单的过滤词：组件一多，菜单需要能搜 */
		char m_AddComponentFilter[64]{};
		/* 顶栏「新建实体」菜单的过滤词（与添加组件菜单同款：打开即清空并聚焦） */
		char m_NewEntityFilter[64]{};
		/* 层级面板的过滤词（空 = 不过滤） */
		char m_EntityFilter[64]{};
		/* 类型筛选（底栏左端的下拉） */
		TypeFilter m_TypeFilter{ TypeFilter::All };
		/* 顶栏「新建」菜单的弹层 ID：在面板根作用域上算一次，
		 * 按钮（开）与菜单（画）两边按同一个 ID 对接 */
		ImGuiID m_PopupNewEntity{ 0 };
	};
}

