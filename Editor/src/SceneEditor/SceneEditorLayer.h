#pragma once
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
		[[nodiscard]] bool CanUndo() const { return m_CommandStack.CanUndo(); }
		[[nodiscard]] bool CanRedo() const { return m_CommandStack.CanRedo(); }
		[[nodiscard]] const char* GetUndoLabel() const { return m_CommandStack.GetUndoLabel(); }
		[[nodiscard]] const char* GetRedoLabel() const { return m_CommandStack.GetRedoLabel(); }

		void Active(bool active = true) { m_IsActivated = active; }
		bool IsActivated() const { return m_IsActivated; }

		void SetGizmoType(int type) { m_GizmoType = type; }
		void SetPlayMode(PlayMode mode) { m_PlayMode = mode; }

	private:
		/* 更新视口 */
		void UpdateViewport();
		/* 响应键盘 */
		bool OnKeyPressed(class KeyPressedEvent* event);
		/* 响应鼠标 */
		bool OnMouseButtonPressed(class MouseButtonPressedEvent* event);
		/* 切换运行模式 */
		void OnPlayModeChanged();
		/* 响应拖拽文件到主窗口 */
		void OnDragItemToScene(const std::filesystem::path& path);

		/* 显示主场景视口 */
		void ShowSceneViewportUI();
		/* 显示渲染统计信息 */
		void ShowStatisticInfoUI();
		/* 选中Entity时显示操作Gizmo */
		void ShowOperationGizmoUI();

		/* 鼠标选中Entity时的响应 */
		void CheckMouseSelectEntity();

		/* 编辑器相机 */
		UniquePtr<EditorCamera> m_pEditorCamera{ nullptr };

		/* 主场景 */
		SharedPtr<Scene> m_pMainScene;
		/* 当前场景路径 */
		std::string m_ActiveScenePath{};
		/* 场景实体管理窗口 */
		SceneHierarchy m_SceneHierarchy;
		/* 编辑历史：所有改动经命令栈落地 */
		CommandStack m_CommandStack;
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
		/* 默认为编辑模式 */
		PlayMode m_PlayMode{ PlayMode::Edit };
		/* 当前编辑器是否被启用 */
		bool m_IsActivated{ true };
	};
}
