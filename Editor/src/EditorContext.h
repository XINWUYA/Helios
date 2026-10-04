#pragma once
#include "EditorCommon.h"

namespace Helios
{
	class SceneEditorLayer;
	class ModelEditorLayer;

	/* 编辑器共享上下文：面板之间唯一的通信通道。「按名字查 Layer + 下转型」集中在本实现里，
	 * 调用方只依赖显式接口。 */
	class EditorContext
	{
	public:
		EditorContext() = default;
		~EditorContext() = default;

		/* ---- 场景文档操作：菜单 / 快捷键 / 工具栏共用同一实现 ---- */
		void NewScene();
		void ImportScene();
		void SaveScene();
		void SaveSceneAs();

		/* ---- 编辑历史：所有编辑改动经这条通道撤销 / 重做 ---- */
		bool Undo();
		bool Redo();
		[[nodiscard]] bool CanUndo();
		[[nodiscard]] bool CanRedo();
		[[nodiscard]] const char* GetUndoLabel();
		[[nodiscard]] const char* GetRedoLabel();

		/* ---- 运行 / Gizmo 状态（本类持有权威值，再转发给场景面板）---- */
		[[nodiscard]] int GetGizmoType() const { return m_GizmoType; }
		void SetGizmoType(int type);

		[[nodiscard]] PlayMode GetPlayMode() const { return m_PlayMode; }
		void SetPlayMode(PlayMode mode);

		/* ---- 面板显隐：整层启用 ---- */
		[[nodiscard]] bool IsSceneEditorActive();
		void SetSceneEditorActive(bool active);
		[[nodiscard]] bool IsModelEditorActive();
		void SetModelEditorActive(bool active);

		/* ---- 视口窗口显隐：与「整层启用」分开，是窗口右上角关闭按钮的恢复入口 ---- */
		[[nodiscard]] bool IsSceneViewportVisible();
		void SetSceneViewportVisible(bool visible);
		[[nodiscard]] bool IsModelViewportVisible();
		void SetModelViewportVisible(bool visible);

		/* 模型导入（ModelEditor 面板） */
		void ImportModel();

	private:
		/* 惰性解析并缓存：MainEditorLayer 是最先被 Push 的 Layer（见 EditorApp），
		 * 其 OnAttached 执行时 SceneEditorLayer / ModelEditorLayer 尚未创建，
		 * 因此解析不能放在构造或 OnAttached 阶段。 */
		SceneEditorLayer* GetSceneLayer();
		ModelEditorLayer* GetModelLayer();

		SceneEditorLayer* m_pScene{ nullptr };
		ModelEditorLayer* m_pModel{ nullptr };

		int      m_GizmoType{ -1 };
		PlayMode m_PlayMode{ PlayMode::Edit };
	};
}
