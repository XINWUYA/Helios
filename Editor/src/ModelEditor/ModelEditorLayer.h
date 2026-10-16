#pragma once
#include "EditorBuiltinCamera.h"
#include "EditorResourceBrowser.h"
#include "ModelInfo.h"
#include "Helios/Scene/Material.h"

namespace Helios
{
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

		/* 激活窗口 */
		void Active(bool active = true) { m_IsActivated = active; }
		bool IsActivated() const { return m_IsActivated; }

		/* 模型视口窗口显隐（与 m_IsActivated 分开，理由同 SceneEditorLayer） */
		void SetViewportVisible(bool visible) { m_IsViewportVisible = visible; }
		[[nodiscard]] bool IsViewportVisible() const { return m_IsViewportVisible; }

		/* 编辑器相机（自检用：断言打开模型后的取景） */
		[[nodiscard]] EditorCamera* GetEditorCamera() { return m_pEditorCamera.get(); }

		/* 当前模型第 index 段的材质（编辑缓冲那份；自检用：断言预览实体绑的就是它） */
		[[nodiscard]] const SharedPtr<Material>& GetSubModelMaterial(size_t index) const
		{
			return m_pMaterialGroup->GetMaterialByIndex(static_cast<int>(index));
		}

		/* 当前模型第 index 段的材质来源：资产引用路径（空 = 内嵌定义；自检用） */
		[[nodiscard]] std::string GetSubModelMaterialAssetPath(size_t index) const;

		/* 给第 index 段挂材质资产（面板拖拽与自检共用）：反序列化该 .mtl 的内容就地把
		 * 编辑缓冲那份材质覆盖成资产定义 —— 对象地址不变，预览与参数行立即跟随；
		 * 条目记下引用，导出写引用形态（模型跟随材质资产变化） */
		bool AssignSubModelMaterial(size_t index, const std::string& absolute_path);

		/* 子模型材质的参数写回（面板回调与自检共用）：改动落编辑缓冲；挂着资产引用的
		 * 随之转内嵌（与场景属性面板「编辑即实例」同构，导出写全量定义而不是引用） */
		void WriteSubModelMaterialParam(size_t index, ParamType type, const std::string& name, const std::any& value);

		/* 导入模型（弹窗选文件） */
		void ImportModel();
		/* 在模型编辑器里打开模型（属性面板 / 资源面板入口）：交换格式先走导入链转成 .mesh，
		 * .mesh 直接打开；成功后把编辑器与视口窗口一并唤回 */
		void OpenModel(const std::string& path);
		/* 保存模型（.mesh & .mtl）：几何不可编辑 —— .mesh 原样拷贝，材质与贴图写到目标目录 */
		void ExportMeshAndMtl();
		/* 保存到指定路径（弹窗版与自检共用）：.mesh 原样拷贝；材质写同名 .mtl —— 内嵌条目
		 * 全量写出、引用条目只写 Asset；贴图按后缀一并拷到目标目录 */
		bool SaveModelTo(const std::string& mesh_file_path);

	private:
		/* 打开模型文件（弹窗导入 / 拖入 / OpenModel 共用）：.mesh 直接加载，
		 * 交换格式（obj / fbx / gltf / glb / dae / stl）先经内核导入链转换到编辑器工作目录再加载 */
		bool LoadModelFile(const std::string& path);
		/* 加载 .mesh：对"场景里在用的那份资产"打开 —— 网格段与槽表材质来自资产本身，
		 * 材质克隆进编辑缓冲；几何不重建，直接用加载好的对象 */
		bool LoadMeshModel(const std::string& path);
		/* 把当前模型放进编辑器场景（只留一个模型实体）；
		 * 相机取景延到视口尺寸就绪的下一帧（m_AutoFocusPending） */
		void ShowModelInScene();

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
		/* 预览工具（左上角三个图标按钮）：保存模型 / 线框预览 / 重置视图，不随面板收起 */
		void ShowPreviewTools();
		/* 模型信息：基本信息卡 + 每个子模型一张卡 */
		void ShowModelBaseInfoCard();
		void ShowSubModelCards();
		/* 子模型卡的材质引用行：显示当前来源（资产文件名 / embedded），拖 .mtl 落上来即挂载 */
		void DrawSubModelMaterialRefRow(int index, const MaterialEntry& entry);
		/* 材质被就地编辑：挂着的资产引用转内嵌（导出随之写全量定义） */
		void DetachSubModelMaterialAsset(size_t index);
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

		/* 当前模型 */
		SharedPtr<Model> m_pModel{ nullptr };
		/* 当前模型对应的材质组，其中每个Material对应一个子模型 */
		SharedPtr<MaterialGroup> m_pMaterialGroup{ nullptr };

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
		/* 线框预览开关（推给相机，见 EditorCamera::SetWireframeOverlay） */
		bool m_IsWireframeEnabled{ false };
		/* 打开模型后待取景：视口尺寸就绪的下一帧再按包围球聚焦（见 ShowModelInScene） */
		bool m_AutoFocusPending{ false };
	};
}
