#include "Pch.h"
#include "EditorApp.h"
#include "MainEditor/MainEditorLayer.h"
#include "SceneEditor/SceneEditorLayer.h"
#include "ModelEditor/ModelEditorLayer.h"

namespace Helios
{
	EditorApp::EditorApp() : Application("Editor", 1920, 1080, true)
	{
		PushLayer(CreateSharedPtr<MainEditorLayer>()); /* 要先添加 */
		PushLayer(CreateSharedPtr<SceneEditorLayer>());
		PushLayer(CreateSharedPtr<ModelEditorLayer>());
	}
}
