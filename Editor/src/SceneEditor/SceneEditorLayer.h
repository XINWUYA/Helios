#pragma once
#include <functional>
#include "SceneHierarchy.h"
#include "EditorBuiltinCamera.h"
#include "EditorCommon.h"
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

		void SetGizmoType(int type) { m_GizmoType = type; }
		void SetPlayMode(PlayMode mode) { m_PlayMode = mode; }

		/* 资源定位通道（跨面板能力）：层级面板画材质卡的贴图，点击要定位到资源浏览器。
		 * 面板不查 Layer —— 由装配层（EditorApp）接线后直接转发给层级面板。 */
		using AssetRevealFunc = std::function<void(const std::string&)>;
		void SetAssetRevealFunc(AssetRevealFunc func);

		/* 资源选中通道（跨面板能力，反方向）：资源浏览器选中资源 → 属性面板显示它的详情。
		 * 同样由装配层接线，转发给层级面板（属性面板与层级树同属一个面板实现）。 */
		void SetAssetSelection(const std::vector<AssetSelectionEntry>& selection);

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
		/* 响应键盘 */
		bool OnKeyPressed(class KeyPressedEvent* event);
		/* 响应鼠标 */
		bool OnMouseButtonPressed(class MouseButtonPressedEvent* event);
		/* 响应鼠标滚轮（缩放 / 飞行速度） */
		bool OnMouseScrolled(class MouseScrolledEvent* event);
		/* 切换运行模式 */
		void OnPlayModeChanged();
		/* 响应拖拽文件到主窗口 */
		void OnDragItemToScene(const std::filesystem::path& path);

		/* 显示主场景视口 */
		void ShowSceneViewportUI();
		/* 显示渲染统计信息 */
		void ShowStatisticInfoUI();
		/* GPU 计时卡片（开关 + 逐层耗时表） */
		void ShowGPUTimingsCard();
		/* 选中Entity时显示操作Gizmo */
		void ShowOperationGizmoUI();
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
		/* Gizmo 拖拽进行中（拖拽期间的逐帧改动合并为一条历史） */
		bool m_IsGizmoDragging{ false };
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
		/* 视图指示器手势：按下沿命中的轴盘序号（-1 = 未命中）；累计位移用于区分点击 / 拖拽 */
		int32_t m_ViewGizmoPressedDisc{ -1 };
		float m_ViewGizmoDraggedDistance{ 0.0f };
		/* 默认为编辑模式 */
		PlayMode m_PlayMode{ PlayMode::Edit };
		/* 当前编辑器是否被启用 */
		bool m_IsActivated{ true };
		/* 场景视口窗口是否显示 */
		bool m_IsViewportVisible{ true };
	};
}
