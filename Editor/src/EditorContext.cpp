#include "Pch.h"
#include "EditorContext.h"
#include "SceneEditor/SceneEditorLayer.h"
#include "ModelEditor/ModelEditorLayer.h"
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

	void EditorContext::SetGizmoType(int type)
	{
		m_GizmoType = type;
		if (auto* layer = GetSceneLayer())
			layer->SetGizmoType(type);
	}

	void EditorContext::SetPlayMode(PlayMode mode)
	{
		m_PlayMode = mode;
		if (auto* layer = GetSceneLayer())
			layer->SetPlayMode(mode);
	}

	bool EditorContext::IsSceneEditorActive()
	{
		const auto* layer = GetSceneLayer();
		return layer != nullptr && layer->IsActivated();
	}

	void EditorContext::SetSceneEditorActive(bool active)
	{
		if (auto* layer = GetSceneLayer())
			layer->Active(active);
	}

	bool EditorContext::IsModelEditorActive()
	{
		const auto* layer = GetModelLayer();
		return layer != nullptr && layer->IsActivated();
	}

	void EditorContext::SetModelEditorActive(bool active)
	{
		if (auto* layer = GetModelLayer())
			layer->Active(active);
	}

	bool EditorContext::IsSceneViewportVisible()
	{
		const auto* layer = GetSceneLayer();
		return layer != nullptr && layer->IsViewportVisible();
	}

	void EditorContext::SetSceneViewportVisible(bool visible)
	{
		if (auto* layer = GetSceneLayer())
			layer->SetViewportVisible(visible);
	}

	bool EditorContext::IsModelViewportVisible()
	{
		const auto* layer = GetModelLayer();
		return layer != nullptr && layer->IsViewportVisible();
	}

	void EditorContext::SetModelViewportVisible(bool visible)
	{
		if (auto* layer = GetModelLayer())
			layer->SetViewportVisible(visible);
	}

	void EditorContext::ImportModel()
	{
		if (auto* layer = GetModelLayer())
			layer->ImportModel();
	}
}
