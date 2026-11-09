#include "Pch.h"
#include "EditorContext.h"
#include "Panels/SceneEditor/SceneEditorLayer.h"
#include "Panels/ModelEditor/ModelEditorLayer.h"
#include "Helios/Application/Application.h"

namespace Helios
{
	/* GetLayerByName 按值返回 SharedPtr<ILayer>，未命中为空指针；
	 * 用 dynamic_cast 做安全下转（ILayer 为多态类型）。 */
	SceneEditorLayer* EditorContext::GetSceneLayer()
	{
		if (m_pScene == nullptr)
		{
			const SharedPtr<ILayer> layer = Application::Instance()->GetLayerByName("SceneEditorLayer");
			m_pScene = dynamic_cast<SceneEditorLayer*>(layer.get());
		}
		return m_pScene;
	}

	ModelEditorLayer* EditorContext::GetModelLayer()
	{
		if (m_pModel == nullptr)
		{
			const SharedPtr<ILayer> layer = Application::Instance()->GetLayerByName("ModelEditorLayer");
			m_pModel = dynamic_cast<ModelEditorLayer*>(layer.get());
		}
		return m_pModel;
	}

	void EditorContext::NewScene()
	{
		if (auto* layer = GetSceneLayer())
			layer->NewScene();
	}

	void EditorContext::ImportScene()
	{
		if (auto* layer = GetSceneLayer())
			layer->ImportScene();
	}

	void EditorContext::OpenScene(const std::string& path)
	{
		if (auto* layer = GetSceneLayer())
			layer->OpenScene(path);
	}

	void EditorContext::OpenAnimationClip(const std::string& path)
	{
		if (auto* layer = GetSceneLayer())
			layer->OpenAnimationClip(path);
	}

	bool EditorContext::WantsTimelineArrowKeys()
	{
		const auto* layer = GetSceneLayer();
		return layer != nullptr && layer->WantsTimelineArrowKeys();
	}

	void EditorContext::SaveScene()
	{
		if (auto* layer = GetSceneLayer())
			layer->SaveScene();
	}

	void EditorContext::SaveSceneAs()
	{
		if (auto* layer = GetSceneLayer())
			layer->SaveSceneAs();
	}

	/* 与层级面板根节点的脏标记同源：主壳层不做第二份计算 */
	bool EditorContext::IsSceneDirty()
	{
		const auto* layer = GetSceneLayer();
		return layer != nullptr && layer->IsSceneDirty();
	}

	bool EditorContext::Undo()
	{
		auto* layer = GetSceneLayer();
		return layer != nullptr && layer->Undo();
	}

	bool EditorContext::Redo()
	{
		auto* layer = GetSceneLayer();
		return layer != nullptr && layer->Redo();
	}

	bool EditorContext::CanUndo()
	{
		const auto* layer = GetSceneLayer();
		return layer != nullptr && layer->CanUndo();
	}

	bool EditorContext::CanRedo()
	{
		const auto* layer = GetSceneLayer();
		return layer != nullptr && layer->CanRedo();
	}

	const char* EditorContext::GetUndoLabel()
	{
		const auto* layer = GetSceneLayer();
		return layer != nullptr ? layer->GetUndoLabel() : nullptr;
	}

	const char* EditorContext::GetRedoLabel()
	{
		const auto* layer = GetSceneLayer();
		return layer != nullptr ? layer->GetRedoLabel() : nullptr;
	}

	void EditorContext::Execute(UniquePtr<ICommand> command)
	{
		if (auto* layer = GetSceneLayer())
			layer->ExecuteAssetCommand(std::move(command));
	}

	void EditorContext::SetGizmoType(int type)
	{
		m_GizmoType = type;
		if (auto* layer = GetSceneLayer())
			layer->SetGizmoType(type);
	}

	void EditorContext::SetGizmoLocalSpace(bool local)
	{
		m_GizmoLocalSpace = local;
		if (auto* layer = GetSceneLayer())
			layer->SetGizmoLocalSpace(local);
	}

	bool EditorContext::IsGizmoDragging()
	{
		const auto* layer = GetSceneLayer();
		return layer != nullptr && layer->IsGizmoDragging();
	}

	PlayState EditorContext::GetPlayState()
	{
		const auto* layer = GetSceneLayer();
		return layer != nullptr ? layer->GetPlayState() : PlayState::Edit;
	}

	void EditorContext::SetPlayState(PlayState state)
	{
		if (auto* layer = GetSceneLayer())
			layer->SetPlayState(state);
	}

	void EditorContext::StepFrame()
	{
		if (auto* layer = GetSceneLayer())
			layer->StepFrame();
	}

	bool EditorContext::IsGameViewFocused()
	{
		const auto* layer = GetSceneLayer();
		return layer != nullptr && layer->IsGameViewFocused();
	}

	const char* EditorContext::GetPlayHint()
	{
		const auto* layer = GetSceneLayer();
		return layer != nullptr ? layer->GetPlayHint() : nullptr;
	}

	void EditorContext::ReloadScripts(const std::function<bool()>& swap_module)
	{
		if (auto* layer = GetSceneLayer())
		{
			layer->ReloadScripts(swap_module);
			return;
		}

		/* 场景面板还没建：至少把库换掉 */
		if (swap_module)
			swap_module();
	}

	const EditorCamera* EditorContext::GetEditorCamera()
	{
		const auto* layer = GetSceneLayer();
		return layer != nullptr ? layer->GetEditorCamera() : nullptr;
	}

	/* 渲染管线读写都经场景面板落到编辑器相机（权威值在相机上）。
	 * 面板未就绪时读回缺省延迟（相机缺省值），写为无操作。 */
	RenderPipeline EditorContext::GetRenderPipeline()
	{
		const auto* layer = GetSceneLayer();
		return layer != nullptr ? layer->GetRenderPipeline() : RenderPipeline::Deferred;
	}

	void EditorContext::SetRenderPipeline(RenderPipeline pipeline)
	{
		if (auto* layer = GetSceneLayer())
			layer->SetRenderPipeline(pipeline);
	}

	/* 调试视图与渲染管线同一条通道；面板未就绪时读回缺省 None */
	DebugViewMode EditorContext::GetDebugViewMode()
	{
		const auto* layer = GetSceneLayer();
		return layer != nullptr ? layer->GetDebugViewMode() : DebugViewMode::None;
	}

	void EditorContext::SetDebugViewMode(DebugViewMode mode)
	{
		if (auto* layer = GetSceneLayer())
			layer->SetDebugViewMode(mode);
	}

	void EditorContext::ImportModel()
	{
		if (auto* layer = GetModelLayer())
			layer->ImportModel();
	}

	void EditorContext::OpenModel(const std::string& path)
	{
		if (auto* layer = GetModelLayer())
			layer->OpenModel(path);
	}
}
