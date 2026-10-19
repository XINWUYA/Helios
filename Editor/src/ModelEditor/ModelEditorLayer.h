#pragma once
#include <entt/entt.hpp>
#include <functional>
#include "EditorBuiltinCamera.h"
#include "EditorResourceBrowser.h"
#include "ModelInfo.h"
#include "Helios/Scene/AnimationClip.h"
#include "Helios/Scene/Material.h"

namespace Helios
{
	struct AnimatorComponent;

	/* 模型编辑器
	 * ：模型导入
	 * ：材质编辑
	 */
	class ModelEditorLayer final : public ILayer
	{
	public:
		ModelEditorLayer();
		~ModelEditorLayer() override = default;

		void OnAttached() override;
		void OnDetached() override;
		void OnUpdate(float delta_time) override;
		void OnImGuiRender() override;
		void OnEvent(IEvent* event) override;

		/* 资源定位通道（跨面板能力）：材质卡里的贴图点击 → 资源浏览器切目录并选中。
		 * 由装配层接线注入，面板不查其它 Layer。 */
		using AssetRevealFunc = std::function<void(const std::string&)>;
		void SetAssetRevealFunc(AssetRevealFunc func) { m_AssetRevealFunc = std::move(func); }

		/* 激活窗口 */
		void Active(bool active = true) { m_IsActivated = active; }
		bool IsActivated() const { return m_IsActivated; }

		/* 模型视口窗口显隐（与 m_IsActivated 分开，理由同 SceneEditorLayer） */
		void SetViewportVisible(bool visible) { m_IsViewportVisible = visible; }
		[[nodiscard]] bool IsViewportVisible() const { return m_IsViewportVisible; }

		/* 编辑器相机（自检用：断言打开模型后的取景） */
		[[nodiscard]] EditorCamera* GetEditorCamera() { return m_pEditorCamera.get(); }

		/* 当前模型第 index 段的材质（有效材质：实体覆盖 ?: 清单默认；自检用） */
		[[nodiscard]] SharedPtr<Material> GetSubModelMaterial(size_t index) const;

		/* 当前模型第 index 段的材质来源：资产引用路径（空 = 内嵌定义；自检用） */
		[[nodiscard]] std::string GetSubModelMaterialAssetPath(size_t index) const;

		/* 当前模型自带的动画片段（与 .mesh 同目录、按导入命名收集；自检用） */
		[[nodiscard]] size_t GetAnimationClipCount() const { return m_AnimationClips.size(); }
		[[nodiscard]] const SharedPtr<AnimationClip>& GetAnimationClip(size_t index) const
		{
			return m_AnimationClips[index];
		}

		/* 预览实体上挂的动画组件：模型带片段时自动挂好并关联首个 clip（自检用） */
		[[nodiscard]] AnimatorComponent* GetPreviewAnimator();

		/* 给第 index 段挂材质资产（面板拖拽与自检共用）：记成共享覆盖，预览立即跟随；
		 * 不改参数时导出写引用形态（模型跟随材质资产变化） */
		bool AssignSubModelMaterial(size_t index, const std::string& absolute_path);

		/* 子模型材质的参数写回（面板回调与自检共用）：改参数自动转实例，
		 * 导出写全量定义而不是引用（与场景属性面板同一套语义） */
		void WriteSubModelMaterialParam(size_t index, ParamType type, const std::string& name, const std::any& value);

		/* 导入模型（弹窗选文件） */
		void ImportModel();
		/* 在模型编辑器里打开模型（属性面板 / 资源面板入口）：交换格式先走导入链转成 .mesh，
		 * .mesh 直接打开；成功后把编辑器与视口窗口一并唤回 */
		void OpenModel(const std::string& path);
		/* 一键保存模型（.model + .mesh + .mtl + .anim + 贴图）：几何不可编辑 —— .mesh 原样拷贝，
		 * 其余按需写到目标目录 */
		void ExportMeshAndMtl();
		/* 保存到指定路径（弹窗版与自检共用）：几何拷成同名 .mesh；材质写进 <stem>_materials，
		 * 贴图收进 textures，最后写 .model 清单组装 */
		bool SaveModelTo(const std::string& model_file_path);

		/* 单独导出一种文件（面板的导出按钮与自检共用）：只写目标文件，不碰同名的其它资产 */
		bool ExportModelTo(const std::string& mesh_file_path);
		bool ExportMaterialTo(const std::string& material_file_path, int slot_index = 0);
		bool ExportAnimationTo(const std::string& animation_file_path);

	private:
		/* 打开模型文件（弹窗导入 / 拖入 / OpenModel 共用）：.mesh 直接加载，
		 * 交换格式（obj / fbx / gltf / glb / dae / stl）先经内核导入链转换到编辑器工作目录再加载 */
		bool LoadModelFile(const std::string& path);
		/* 加载 .model / .mesh：对"场景里在用的那份资产"打开 —— 网格段与槽表来自资产本身，
		 * 编辑落在槽覆盖上；几何不重建，直接用加载好的对象 */
		bool LoadMeshModel(const std::string& path);
		/* 把当前模型放进编辑器场景（只留一个模型实体）；
		 * 相机取景延到视口尺寸就绪的下一帧（m_AutoFocusPending） */
		void ShowModelInScene();
		/* 收集与 .mesh 同目录、按导入命名（<模型名>_<片段名>.anim）的动画片段 */
		void CollectAnimationClips(const std::string& mesh_path);

		/* 模型编辑窗口：左右分栏 —— 左栏预览铺满（预览工具图标常驻其左上角），
		 * 右栏材质参数面板（可收起）。视口与参数原先各占一个窗口（Model + Model Helper），
		 * 现已合并成一个窗口 */
		void ShowModelWindowUI();
		/* 材质参数面板（右栏）：最上方标题栏显示当前模型名，下面基础信息卡 + 每个子模型一张卡；
		 * 内容只上下滚（常驻竖向滚动条），横向不含可滑区 */
		void ShowMaterialParamsPane();
		/* 面板的折叠开关：打开 / 收起两态画在窗口右上角同一处。
		 * 打开时面板子窗口盖住父窗口的控件，开关必须画在面板里；收起时画在视口上 */
		bool DrawParamsPaneToggle();
		/* 预览工具（左上角三个图标按钮）：保存模型 / Debug View 档位（与场景视口同一份表）/ 重置视图 */
		void ShowPreviewTools();
		/* 模型信息：基本信息卡 + 每个子模型一张卡 */
		void ShowModelBaseInfoCard();
		void ShowSubModelCards();
		/* 动画卡（右栏）：片段选择 + 播放 / 循环 / 速度 / 时间轴 —— 改的是预览实体上的动画组件。
		 * 模型带片段时自动挂好并关联首个 clip，打开就能看动（没有片段时整张卡不出现） */
		void ShowAnimationCard();
		/* 导出卡（右栏）：模型 / 材质 / 动画分开导出，各自选路径 */
		void ShowExportCard();
		/* 导出卡的三个按钮：弹窗选路径后走对应的 ExportTo */
		void ExportModelOnly();
		void ExportMaterialOnly();
		void ExportSelectedAnimation();
		/* 子模型卡的材质引用行：显示当前来源（.mtl 文件名 / 模型默认），拖 .mtl 落上来即挂载 */
		void DrawSubModelMaterialRefRow(int index);
		/* 未导入模型时的空状态提示（按视口区域居中；画在视口输入区之前） */
		void ShowEmptyModelHint(const ImVec2& area_min, const ImVec2& area_size);
		/* 响应拖拽文件到主窗口 */
		void OnDragItemToScene(const std::filesystem::path& path);
		/* 滚轮：指针在视口上时拉近拉远 */
		bool OnMouseScrolled(class MouseScrolledEvent* event);

		/* 默认场景 */
		SharedPtr<Scene> m_pDefaultScene{ nullptr };
		/* 编辑器相机 */
		UniquePtr<EditorCamera> m_pEditorCamera{ nullptr };
		/* 视口范围: x: width_min; y: height_min; z: width_max; w: height_max */
		ViewportRegion m_ViewportRegion{};
		/* 指针停留在视口上（相机导航的起手授权） */
		bool m_IsViewportHovered{ false };

		/* 从文件中加载模型信息 */
		UniquePtr<ModelInfo> m_pModelInfo{ nullptr };

		/* 槽覆盖改动后同步到预览实体（见 OnUpdate）：只在改过的那几帧拷一次 */
		void MarkSlotOverridesDirty() { m_SlotOverridesDirty = true; }

		/* 当前模型 */
		SharedPtr<Model> m_pModel{ nullptr };
		/* 槽覆盖：与场景编辑器的 ModelComponent 同一套语义（改参数自动实例化） */
		std::unordered_map<std::string, MaterialSlotOverride> m_SlotOverrides;
		/* 槽覆盖改过、还没同步到预览实体 */
		bool m_SlotOverridesDirty{ false };
		/* 导出 .mtl 时选中的槽（多槽模型在导出卡里选） */
		int m_ExportSlot{ 0 };
		/* 预览实体：换模型时整棵销毁（骨骼模型的关节是它的子节点） */
		entt::entity m_PreviewEntity{ entt::null };
		/* 当前模型自带的动画片段（与 .mesh 同目录，按文件名排序） */
		std::vector<SharedPtr<AnimationClip>> m_AnimationClips;
		/* 选中的片段下标（-1 = 没有） */
		int m_SelectedClip{ -1 };

		/* 当前窗口是否被激活 */
		bool m_IsActivated{ false };
		/* 模型视口窗口是否显示 */
		bool m_IsViewportVisible{ true };
		/* 材质参数面板是否展开（右上角开关切换） */
		bool m_IsParamsPanelOpen{ true };
		/* 本帧指针是否悬停在面板折叠开关上（收起态它盖在视口角上，要挡掉视口悬停） */
		bool m_IsParamsToggleHovered{ false };
		/* 本帧指针是否悬停在预览工具按钮上（同上，挡掉视口悬停） */
		bool m_IsPreviewToolsHovered{ false };
		/* 本帧悬停的子模型卡索引：对应网格段在预览里高亮（见 EditorCamera::SetHighlightSegment）；
		 * 只有多个子网格时才置位，-1 = 没有悬停 */
		int m_HighlightedSubModel{ -1 };
		/* Shader 选择菜单的可选清单缓存（菜单打开时重扫） */
		std::vector<std::string> m_ShaderOptions;
		/* 贴图点击定位（见 SetAssetRevealFunc；未注入时点击无效果） */
		AssetRevealFunc m_AssetRevealFunc;
		/* 打开模型后待取景：视口尺寸就绪的下一帧再按包围球聚焦（见 ShowModelInScene） */
		bool m_AutoFocusPending{ false };
		/* 打开模型后待开播：关节树由 SkeletonModel 在场景更新里建，等这一帧过去再开播
		 * （提前开播会绑到马上被重建掉的旧关节上，之后再也绑不上） */
		bool m_AnimationStartPending{ false };
	};
}
