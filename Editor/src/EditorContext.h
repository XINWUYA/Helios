#pragma once
#include "EditorCommon.h"
#include "Helios/Command/Command.h"
#include "Helios/Common/Common.h"
#include "Helios/Scene/Camera.h"

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
		/* 打开指定场景文件（资源浏览器双击 .scn）；path 为绝对路径 */
		void OpenScene(const std::string& path);
		/* 双击 .anim：让时间轴独立编辑这个片段（不需要实体） */
		void OpenAnimationClip(const std::string& path);
		/* 方向键是否归时间轴微调（鼠标在时刻区）：主壳层据此压掉这一帧的导航移动 */
		[[nodiscard]] bool WantsTimelineArrowKeys();
		void SaveScene();
		void SaveSceneAs();

		/* 场景有没有未保存的改动：工具栏的保存按钮据此启用 / 禁用，
		 * 与层级面板根节点的脏标记同一判据（SceneEditorLayer::IsSceneDirty）。 */
		[[nodiscard]] bool IsSceneDirty();

		/* ---- 编辑历史：所有编辑改动经这条通道撤销 / 重做 ---- */
		bool Undo();
		bool Redo();
		[[nodiscard]] bool CanUndo();
		[[nodiscard]] bool CanRedo();
		[[nodiscard]] const char* GetUndoLabel();
		[[nodiscard]] const char* GetRedoLabel();

		/* 往同一条编辑历史里塞一条命令：资源浏览器的新建 / 重命名 / 删除都走这里 —— 所以 Ctrl+Z 对
		 * 文件操作同样有效。命令不涉及场景改动时，自己声明 AffectsSceneDocument() 为假。 */
		void Execute(UniquePtr<ICommand> command);

		[[nodiscard]] bool IsScriptOrderVisible();
		void SetScriptOrderVisible(bool visible);

		/* ---- 运行 / Gizmo 状态（本类持有权威值，再转发给场景面板）---- */
		[[nodiscard]] int GetGizmoType() const { return m_GizmoType; }
		void SetGizmoType(int type);
		/* Gizmo 拖拽进行中：主壳层据此屏蔽快捷键（拖拽期间不切工具、不撤销） */
		/* Gizmo 坐标空间：0 = Local（轴跟实体旋转）、1 = World（轴恒为世界轴） */

		[[nodiscard]] PlayMode GetPlayMode() const { return m_PlayMode; }
		void SetPlayMode(PlayMode mode);

		/* 热重载脚本：运行中的实例按字段迁移到新库，不用退出 Runtime。
		 * swap_module 由调用方给（编译并换库），面板不认识脚本模块。 */
		void ReloadScripts(const std::function<bool()>& swap_module);

		/* ---- 渲染管线（编辑器场景视口）：前向 / 延迟。
		 * 状态在相机的视图上（单一数据源），这里只做通道转发。 ---- */
		[[nodiscard]] RenderPipeline GetRenderPipeline();
		void SetRenderPipeline(RenderPipeline pipeline);

		/* ---- 调试视图（编辑器场景视口）：Surface / Lighting / Analysis 分类见
		 * DebugViewMode。同样落相机视图，这里只做通道转发。 ---- */
		[[nodiscard]] DebugViewMode GetDebugViewMode();
		void SetDebugViewMode(DebugViewMode mode);

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

		/* ---- 渲染图可视化页（Frame Graph）窗口显隐 ---- */
		[[nodiscard]] bool IsFrameGraphVisible();
		void SetFrameGraphVisible(bool visible);

		/* ---- 时间轴窗口显隐 ---- */
		[[nodiscard]] bool IsTimelineVisible();
		void SetTimelineVisible(bool visible);

		/* 模型导入（ModelEditor 面板） */
		void ImportModel();
		/* 在模型编辑器里打开指定模型文件（属性面板的入口走这里；path 为绝对路径） */
		void OpenModel(const std::string& path);

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
