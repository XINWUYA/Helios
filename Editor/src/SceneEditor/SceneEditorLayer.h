#pragma once
#include <functional>
#include <unordered_map>
#include "FrameGraphPanel.h"
#include "RenderStatsPanel.h"
#include "SceneHierarchy.h"
#include "TimelinePanel.h"
#include "EditorBuiltinCamera.h"
#include "EditorCommon.h"
#include "TransformGizmo.h"
#include "Helios/Command/CommandStack.h"

namespace Helios
{
	/* Editor层类 */
	class SceneEditorLayer final : public ILayer
	{
	public:
		SceneEditorLayer();
		~SceneEditorLayer() override = default;

		void OnAttached() override;
		void OnDetached() override;
		void OnUpdate(float delta_time) override;
		void OnImGuiRender() override;
		void OnEvent(IEvent* event) override;

		/* 新建场景 */
		void NewScene();
		/* 导入场景：通过弹窗找到指定场景文件并打开 */
		void ImportScene();
		/* 打开场景文件：加载进一个新场景，成功才切换 ——
		 * 导入对话框 / 拖入 / 资源浏览器双击三条入口共用这一处。 */
		void OpenScene(const std::string& path);
		/* 双击 .anim：加载片段并让时间轴独立编辑它（自动打开时间轴面板并切到前台） */
		void OpenAnimationClip(const std::string& path);

		/* 方向键此刻归不归时间轴（鼠标停在时刻区）：主壳层据此压掉 ImGui 的导航移动 */
		[[nodiscard]] bool WantsTimelineArrowKeys() const;
		/* 保存场景 */
		void SaveScene();
		/* 保存场景到指定路径 */
		void SaveSceneAs();

		/* 编辑历史 */
		bool Undo();
		bool Redo();
		/* 往同一条历史里塞一条命令（资源浏览器的新建 / 重命名 / 删除走这里，
		 * 于是工具栏与菜单的撤销 / 重做对文件操作同样有效） */
		void ExecuteCommand(UniquePtr<ICommand> command) { m_CommandStack.Execute(std::move(command)); }
		[[nodiscard]] bool CanUndo() const { return m_CommandStack.CanUndo(); }
		[[nodiscard]] bool CanRedo() const { return m_CommandStack.CanRedo(); }
		[[nodiscard]] const char* GetUndoLabel() const { return m_CommandStack.GetUndoLabel(); }
		[[nodiscard]] const char* GetRedoLabel() const { return m_CommandStack.GetRedoLabel(); }

		/* 场景有没有未保存的改动（层级面板根节点据此显示脏标记）。
		 * 主判据是编辑历史的位置与上次保存时是否一致 —— 撤销回保存点即视为干净；
		 * 少量不走命令栈的改动（如拖入模型）由 m_HasUnrecordedChange 兜底。 */
		[[nodiscard]] bool IsSceneDirty() const;

		void Active(bool active = true) { m_IsActivated = active; }
		bool IsActivated() const { return m_IsActivated; }

		/* 场景视口窗口显隐（窗口右上角关闭按钮 / View 菜单）。
		 * 与 m_IsActivated 分开：关掉视口只隐藏这一个窗口，
		 * 层级、属性、统计面板与场景数据都不受影响。 */
		void SetViewportVisible(bool visible) { m_IsViewportVisible = visible; }
		[[nodiscard]] bool IsViewportVisible() const { return m_IsViewportVisible; }

		/* 渲染图可视化页（Frame Graph）窗口显隐（与视口同一套模式：窗口关闭按钮 / View 菜单） */
		void SetFrameGraphVisible(bool visible) { m_IsFrameGraphVisible = visible; }
		[[nodiscard]] bool IsFrameGraphVisible() const { return m_IsFrameGraphVisible; }

		/* 时间轴窗口显隐（同上） */
		void SetTimelineVisible(bool visible) { m_IsTimelineVisible = visible; }
		[[nodiscard]] bool IsTimelineVisible() const { return m_IsTimelineVisible; }

		/* 脚本执行顺序总表窗口显隐（Settings 菜单打开，默认不占布局） */
		void SetScriptOrderVisible(bool visible) { m_IsScriptOrderVisible = visible; }
		[[nodiscard]] bool IsScriptOrderVisible() const { return m_IsScriptOrderVisible; }

		void SetGizmoType(int type) { m_GizmoType = type; }
		/* 操作 gizmo 的轴基：true = 实体局部轴，false = 世界轴 */
		void SetGizmoLocalSpace(bool local) { m_GizmoLocalSpace = local; }
		[[nodiscard]] bool IsGizmoLocalSpace() const { return m_GizmoLocalSpace; }
		/* 操作 gizmo 拖拽中 / 指针停在手柄上：主壳层据此屏蔽快捷键与相机导航
		 * （与原 ImGuizmo::IsUsing / IsOver 同义） */
		[[nodiscard]] bool IsGizmoDragging() const { return m_GizmoDrag.Active; }
		[[nodiscard]] bool IsGizmoHovered() const { return m_GizmoHoveredHandle >= 0; }
		void SetPlayMode(PlayMode mode);
		/* 热重载脚本：运行中的实例按字段迁移到新库，不需要退出 Runtime */
		void ReloadScripts(const std::function<bool()>& swap_module);

		/* 渲染管线（场景视口）：前向 / 延迟 —— 状态在编辑器相机的视图上（相机转发） */
		void SetRenderPipeline(RenderPipeline pipeline) { m_pEditorCamera->SetRenderPipeline(pipeline); }
		[[nodiscard]] RenderPipeline GetRenderPipeline() const { return m_pEditorCamera->GetRenderPipeline(); }

		/* 调试视图（场景视口）：Surface / Lighting / Analysis 分类见 DebugViewMode ——
		 * 状态同样在相机视图上（相机转发） */
		void SetDebugViewMode(DebugViewMode mode) { m_pEditorCamera->SetDebugViewMode(mode); }
		[[nodiscard]] DebugViewMode GetDebugViewMode() const { return m_pEditorCamera->GetDebugViewMode(); }

		/* 资源定位通道（跨面板能力）：层级面板画材质卡的贴图，点击要定位到资源浏览器。
		 * 面板不查 Layer —— 由装配层（EditorApp）接线后直接转发给层级面板。 */
		using AssetRevealFunc = std::function<void(const std::string&)>;
		void SetAssetRevealFunc(AssetRevealFunc func);

		/* 资源选中通道（跨面板能力，反方向）：资源浏览器选中资源 → 属性面板显示它的详情。
		 * 同样由装配层接线，转发给层级面板（属性面板与层级树同属一个面板实现）。 */
		void SetAssetSelection(const std::vector<AssetSelectionEntry>& selection);

		/* 模型编辑器入口（跨面板能力）：属性面板 Model 卡里的「Open in Model Editor」按钮点击 →
		 * 在模型编辑器里打开该模型。同样由装配层接线（面板不查 Layer）。 */
		using ModelEditorFunc = std::function<void(const std::string&)>;
		void SetModelEditorFunc(ModelEditorFunc func);

	private:
		/* 切换到指定场景并重置一切与「旧场景内容」绑定的状态。
		 * 新建 / 打开 / 拖拽导入都必须经过这里，否则容易漏掉其中一项。 */
		void SetActiveScene(const SharedPtr<Scene>& scene, const std::string& path);
		/* 当前场景文件路径的唯一写入口：层级面板根节点显示的场景名跟着它走，
		 * 新建 / 打开 / 首次保存 / 另存为都从这里改，避免两处状态不同步。 */
		void SetActiveScenePath(const std::string& path);
		/* 把「干净点」记在当前编辑历史位置上：保存成功、以及切场景（历史被清空）后调用 */
		void MarkSceneSaved();
		/* 更新视口 */
		void UpdateViewport();
		/* 场景相机（游戏相机）渲染到窗口默认目标：视口区域 = 窗口画布（物理像素）。
		 * 引擎样例由宿主设置这一尺寸；不设则区域恒为 {0,0,0,0}、ScenePass
		 * 以 0×0 视口空跑（FrameGraph 页"空车道"的根因之一） */
		void UpdateSceneCameraViewports();
		/* 收集本帧可视化可选的相机（编辑器相机在前、场景相机随后）：
		 * OnUpdate（抓取目标落位）与 OnImGuiRender（面板列表）共用同一份 */
		void CollectCameraEntries();
		/* 响应键盘 */
		bool OnKeyPressed(class KeyPressedEvent* event);
		/* 响应鼠标 */
		bool OnMouseButtonPressed(class MouseButtonPressedEvent* event);
		/* 响应鼠标滚轮（缩放 / 飞行速度） */
		bool OnMouseScrolled(class MouseScrolledEvent* event);
		/* 切换运行模式 */
		void OnPlayModeChanged();
		/* 响应拖拽文件到主窗口（资源浏览器内部拖拽） */
		void OnDragItemToScene(const std::filesystem::path& path);
		/* 响应系统文件拖入（Finder 把文件拖到窗口上）：落点在场景视口内才算「拖进场景」 */
		bool OnFileDrop(class WindowFileDropEvent* event);
		/* 拖入的模型文件进场景：.obj 先收进资产目录并转成 .mesh + .mtl（已在项目里就原地转换），
		 * .mesh 直接收录引用；随后建一个引用它的实体。返回是否成功处理了这个文件。 */
		bool ImportModelFile(const std::filesystem::path& path);
		/* 建一个挂指定 .mesh 的模型实体（导入 / 拖入共用）：加载失败返回空句柄 */
		Entity CreateModelEntity(const std::string& absolute_mesh_path);

		/* 显示主场景视口 */
		void ShowSceneViewportUI();
		/* 脚本执行顺序总表：全场景挂载点按 Order 排，行内可改 */
		void DrawScriptOrderWindow();
		/* 顺序表的草稿落到组件上：按实体一条命令，可撤销 */
		void ApplyScriptOrderDraft();
		/* 显示渲染统计信息 */
		void ShowStatisticInfoUI();
		/* 选中Entity时显示操作Gizmo */
		void ShowOperationGizmoUI();
		/* 收尾一次 Gizmo 拖拽：关掉合并窗口，整段拖拽在历史里只留一条 */
		void EndGizmoDrag();
		/* 视口右上角的视图指示器：六轴盘，点击切视角 / 拖拽转视角 */
		void ShowViewGizmoUI();

		/* 鼠标选中Entity时的响应 */
		void CheckMouseSelectEntity();

		/* F：把视角聚焦到选中实体（按模型包围球取景） */
		void FocusSelectedEntity();

		/* 编辑器相机 */
		UniquePtr<EditorCamera> m_pEditorCamera{ nullptr };

		/* 主场景 */
		SharedPtr<Scene> m_pMainScene;
		/* 当前场景路径（只经 SetActiveScenePath 改） */
		std::string m_ActiveScenePath{};
		/* 场景实体管理窗口 */
		SceneHierarchy m_SceneHierarchy;
		/* 编辑历史：所有改动经命令栈落地 */
		CommandStack m_CommandStack;
		/* 上次保存（或加载）时的场景改动条数：与当前条数不一致即表示有未保存的改动。
		 * 用条数而不是历史位置：资源操作（新建 / 重命名 / 删除）也入同一条历史，
		 * 它们不改场景内容，按位置比对会被它们带出假的"未保存"（见 CommandStack::GetSceneEditCount）。 */
		size_t m_SavedSceneEditCount{ 0 };
		/* 发生过不走命令栈的场景改动（如拖入模型） */
		bool m_HasUnrecordedChange{ false };
		/* 操作 Gizmo：本帧布局（拾取 / 拖拽共用）、悬停手柄与拖拽状态。
		 * 几何与数学在 TransformGizmo.h，这里只保存跨帧状态。 */
		TransformGizmo::Layout m_GizmoLayout{};
		TransformGizmo::Drag m_GizmoDrag{};
		int32_t m_GizmoHoveredHandle{ -1 };
		/* 拖拽起点快照：每帧都从它重算世界矩阵，长距离拖拽不会累积误差 */
		Entity m_GizmoEntity{};
		TransformComponent m_GizmoStartTransform{};
		glm::mat4 m_GizmoStartWorld{ 1.0f };
		/* 选中实体 */
		Entity m_HoveredEntity;
		/* 视口范围: x: width_min; y: height_min; z: width_max; w: height_max */
		ViewportRegion m_ViewportRegion{};
		/* 视口窗口被激活 */
		bool m_IsViewportFocused{ false };
		/* 鼠标停留在视口上 */
		bool m_IsViewportHovered{ false };
		/* 移动，旋转，缩放UI */
		int m_GizmoType = -1;
		bool m_GizmoLocalSpace{ true };
		/* 视图指示器手势：按下沿命中的轴盘序号（-1 = 未命中）；累计位移用于区分点击 / 拖拽 */
		int32_t m_ViewGizmoPressedDisc{ -1 };
		float m_ViewGizmoDraggedDistance{ 0.0f };
		/* 默认为编辑模式 */
		PlayMode m_PlayMode{ PlayMode::Edit };
		/* 进 Runtime 前的场景快照：退出时按它还原，空路径 = 没有快照 */
		std::filesystem::path m_RuntimeSnapshotPath;
		/* 当前编辑器是否被启用 */
		bool m_IsActivated{ true };
		/* 场景视口窗口是否显示 */
		bool m_IsViewportVisible{ true };
		/* 渲染图可视化页：依赖图 + 各 Pass 的中间渲染结果（抓取开关由面板自持） */
		FrameGraphPanel m_FrameGraphPanel;
		/* 渲染图可视化页窗口是否显示 */
		bool m_IsFrameGraphVisible{ true };
		/* 时间轴面板与窗口显隐 */
		TimelinePanel m_TimelinePanel;
		bool m_IsTimelineVisible{ true };
		/* 打开片段后要求把时间轴页签切到前台（下一帧 SetWindowFocus） */
		bool m_FocusTimelineRequested{ false };
		/* 脚本执行顺序总表窗口是否显示（Settings 菜单打开） */
		bool m_IsScriptOrderVisible{ false };
		/* 顺序表的草稿：key = 实体句柄 << 32 | 槽位下标，点 Apply 才写进组件 */
		std::unordered_map<uint64_t, int> m_ScriptOrderDraft;
		/* 本帧可视化可选的相机（渲染图与相机同生命周期，指针跨帧稳定） */
		std::vector<FrameGraphPanel::CameraEntry> m_CameraEntries;
		/* 渲染统计面板（每帧 CPU / GPU 耗时；绘制逻辑在面板自身） */
		RenderStatsPanel m_RenderStatsPanel;
	};
}
