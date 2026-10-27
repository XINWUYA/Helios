#include "Pch.h"
#include "EditorApp.h"
#include "LogBuffer.h"
#include "MainEditor/MainEditorLayer.h"
#include "SceneEditor/SceneEditorLayer.h"
#include "ModelEditor/ModelEditorLayer.h"
#include "ProjectLauncher/RecentProjects.h"
#include "Helios/Common/PathUtils.h"
#include "Helios/Common/ProjectConfig.h"
#include "Helios/ImGui/EditorTheme.h"

namespace Helios
{
	namespace
	{
		constexpr const char* kBaseTitle = "Helios Editor";

		/* 标题带项目名：同时开几个项目，任务栏里一眼分得清 */
		std::string BuildWindowTitle(const std::string& project_name)
		{
			return std::string(kBaseTitle) + " - " + project_name;
		}

		/* 编辑器界面外观：字体（EditorRes/fonts）+ 暗色主题 —— 由编辑器自己注入，
		 * 引擎不预设编辑器外观（见 ImGuiLayer::StyleInstaller）。 */
		void InstallEditorUiStyle(ImGuiIO& io, float content_scale)
		{
			const auto fonts = EditorTheme::SetupFonts(io, ABSOLUTE_PATH("EditorRes/fonts"), content_scale);
			if (fonts.Regular == nullptr)
			{
				/* 字体缺失的回退：ImGui 至少要有一份可用字体，否则第一帧断言 */
				CORE_LOG_ERROR("Failed to load editor fonts: EditorRes/fonts");
				io.Fonts->AddFontDefault();
			}
			EditorTheme::ApplyDark();
		}
	}

	EditorApp::EditorApp(bool show_launcher)
		: Application(kBaseTitle, 1920, 1080, true, &InstallEditorUiStyle)
	{
		/* 日志窗口的捕获通道：在 Logger::Init()（main）之后、任何层绘出之前接上 */
		AttachLogCapture();

		/* 启动页是整屏覆盖层：编辑器层在后面建起来的那几帧，它还得盖在最上面。
		 * 命令行指定了工程的话，也先进这一页 —— 建层那几秒有一块进度可看，不至于空窗。 */
		m_pLauncher = CreateSharedPtr<ProjectLauncherLayer>();
		PushOverlay(m_pLauncher);

		if (!show_launcher)
			m_pLauncher->RequestOpen(g_ProjectPath);
	}

	void EditorApp::OnFrameEnd()
	{
		if (m_pLauncher == nullptr)
			return;

		/* 加载中：一帧推进一个阶段（层的构建都在主线程上，没法并行） */
		if (m_LoadStep != LoadStep::None)
		{
			StepLoading();
			return;
		}

		const ProjectLauncherLayer::Decision decision = m_pLauncher->TakeDecision();
		if (decision.Kind == ProjectLauncherLayer::Decision::Kind::None)
			return;

		if (decision.Kind == ProjectLauncherLayer::Decision::Kind::Quit)
		{
			Close();
			return;
		}

		if (!SetProjectPath(decision.ProjectPath))
		{
			m_pLauncher->ShowError("Failed to open project: " + PathToUtf8(decision.ProjectPath));
			return;
		}

		/* 布局跟项目走：先换 ini，编辑器层的窗口才会落进这个项目自己的停靠数据 */
		if (m_pImGuiLayer != nullptr)
			m_pImGuiLayer->RebindLayoutToProject();

		const ProjectConfig config = LoadProjectConfig();
		m_pWindow->SetWindowTitle(BuildWindowTitle(config.Name));

		/* 交给加载页：下一帧开始按阶段建层 */
		m_pLauncher->BeginLoading(config.Name);
		m_pLauncher->SetLoadingStage("Building editor layout", 0.10f);
		m_LoadStep = LoadStep::EditorShell;
	}

	void EditorApp::StepLoading()
	{
		switch (m_LoadStep)
		{
		case LoadStep::EditorShell:
			/* 外壳：编辑器设置、脚本模块（新项目第一次打开要现编一次） */
			CreateEditorShell();
			m_pLauncher->SetLoadingStage("Loading scene resources", 0.45f);
			m_LoadStep = LoadStep::SceneEditor;
			break;

		case LoadStep::SceneEditor:
			/* 场景：建场景、打开启动场景（模型 / 贴图 / 着色器都在这一步） */
			CreateSceneEditor();
			m_pLauncher->SetLoadingStage("Preparing model editor", 0.78f);
			m_LoadStep = LoadStep::ModelEditor;
			break;

		case LoadStep::ModelEditor:
			CreateModelEditor();
			m_pLauncher->SetLoadingStage("Ready", 1.0f);
			m_LoadStep = LoadStep::Finish;
			break;

		case LoadStep::Finish:
			RecordRecentProject();
			m_LoadStep = LoadStep::None;
			m_pLauncher->EndLoading();
			PopOverlay(m_pLauncher);
			m_pLauncher.reset();
			break;

		default:
			break;
		}
	}

	void EditorApp::CreateEditorShell()
	{
		m_pMainLayer = CreateSharedPtr<MainEditorLayer>();
		PushLayer(m_pMainLayer); /* 要先添加 */
	}

	void EditorApp::CreateSceneEditor()
	{
		m_pSceneLayer = CreateSharedPtr<SceneEditorLayer>();

		/* 层级面板的材质卡：点击贴图要定位到资源浏览器 ——
		 * 两层互不认识对方，通道在装配层接上（面板不查 Layer）。 */
		m_pSceneLayer->SetAssetRevealFunc([main = m_pMainLayer](const std::string& relative_path)
		{
			main->RevealAsset(relative_path);
		});

		/* 属性面板 Model 卡里的「Open in Model Editor」按钮：点了就在模型编辑器里打开该模型
		 * （同一条接线思路） */
		m_pSceneLayer->SetModelEditorFunc([main = m_pMainLayer](const std::string& path)
		{
			main->OpenModel(path);
		});

		/* 反方向的同一条思路：资源浏览器选中资源 → 属性面板显示它的详细内容。
		 * 选中项由浏览器在帧末发布（含"同一项被再次点选"），点实体即切回组件视图。 */
		m_pMainLayer->SetAssetSelectionSink(EditorResourceBrowser::AssetSelectionSink{
			[scene = m_pSceneLayer](const std::vector<AssetSelectionEntry>& selection)
			{
				scene->SetAssetSelection(selection);
			} });

		PushLayer(m_pSceneLayer);
	}

	void EditorApp::CreateModelEditor()
	{
		m_pModelLayer = CreateSharedPtr<ModelEditorLayer>();

		/* 模型编辑器的材质卡走同一条定位通道（那边也是同一个材质控件） */
		m_pModelLayer->SetAssetRevealFunc([main = m_pMainLayer](const std::string& relative_path)
		{
			main->RevealAsset(relative_path);
		});

		PushLayer(m_pModelLayer);
	}

	void EditorApp::RecordRecentProject()
	{
		/* 记进最近打开：启动页下次直接点开 */
		RecentProjects recents;
		recents.Load();
		recents.Add(g_ProjectPath);
	}

	UniquePtr<Application> CreateEditorApplication(bool show_launcher)
	{
		return CreateUniquePtr<EditorApp>(show_launcher);
	}

	/* 引擎约定的应用入口（默认走启动页） */
	UniquePtr<Application> CreateApplication()
	{
		return CreateEditorApplication(true);
	}
}
