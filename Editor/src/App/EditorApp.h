#pragma once

#include "Helios/Application/Application.h"
#include "Helios/Core/Layer.h"
#include "Panels/ProjectLauncher/ProjectLauncherLayer.h"
#include "Panels/MainEditor/MainEditorLayer.h"
#include "Panels/SceneEditor/SceneEditorLayer.h"
#include "Panels/ModelEditor/ModelEditorLayer.h"

namespace Helios
{
	class EditorApp final : public Application
	{
	public:
		/* show_launcher = true 时先出启动页选工程，选好再建编辑器各层；
		 * 命令行给了 --project 就跳过启动页直接进工程。 */
		EditorApp(bool show_launcher);
		~EditorApp() override = default;

		/* 帧末：启动页的选择、加载进度都在这里推进。
		 * 层的绘制循环正在遍历层列表，那时候不能加层删层。 */
		void OnFrameEnd() override;

	private:
		/* 打开工程时按阶段建层：一帧只建一层，加载页的进度条才走得动 */
		enum class LoadStep
		{
			None,
			EditorShell,
			SceneEditor,
			ModelEditor,
			Finish
		};

		/* 编辑器各层：顺序有讲究（外壳先于场景，场景先于模型编辑器） */
		void CreateEditorShell();
		void CreateSceneEditor();
		void CreateModelEditor();
		void RecordRecentProject();

		/* 加载中每帧推进一步 */
		void StepLoading();

		/* 启动页（进编辑器后释放） */
		SharedPtr<ProjectLauncherLayer> m_pLauncher;
		SharedPtr<MainEditorLayer> m_pMainLayer;
		SharedPtr<SceneEditorLayer> m_pSceneLayer;
		SharedPtr<ModelEditorLayer> m_pModelLayer;
		LoadStep m_LoadStep{ LoadStep::None };
	};

	/* 编辑器入口：带不带启动页由 main 按命令行决定 */
	UniquePtr<Application> CreateEditorApplication(bool show_launcher);
}
