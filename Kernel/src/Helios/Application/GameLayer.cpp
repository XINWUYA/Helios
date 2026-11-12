#include "Pch.h"
#include "GameLayer.h"

#include "Helios/Application/Application.h"
#include "Helios/Application/ProjectConfig.h"
#include "Helios/Events/ApplicationEvent.h"
#include "Helios/Events/KeyEvent.h"
#include "Helios/Events/MouseEvent.h"
#include "Helios/Renderer/RenderView.h"
#include "Helios/Scene/Camera.h"
#include "Helios/Scene/Components.h"
#include "Helios/Scene/Entity.h"
#include "Helios/Scene/SceneCommon.h"
#include "Helios/Script/ScriptRuntime.h"
#include "Helios/UI/UISystem.h"

namespace Helios
{
	GameLayer::GameLayer(bool output_to_texture)
		: ILayer("GameLayer"), m_OutputToTexture(output_to_texture)
	{
	}

	void GameLayer::OnAttached()
	{
		PROFILE_CPU_FUNCTION();

		/* 画质开关跟编辑器共用 Project.xml，不然草地与 MSAA 的形态对不上 */
		const ProjectConfig config = LoadProjectConfig();
		ApplyProjectSettings(config);

		/* 脚本先挂上：场景里的挂载点按类名建实例，类得先在注册表里 */
		m_Scripts.LoadLatest();

		if (config.StartupScene.empty())
		{
			CORE_LOG_ERROR("Project {} has no startup scene. Set one in the Editor first.", config.Name);
			return;
		}

		m_ScenePath = config.StartupScene;
		const std::string scene_path = ABSOLUTE_PATH(m_ScenePath);
		m_pScene = CreateSharedPtr<Scene>();
		if (!m_pScene->Deserializer(scene_path))
		{
			CORE_LOG_ERROR("Failed to load scene: {}.", scene_path);
			m_pScene = nullptr;
			return;
		}

		m_Camera = static_cast<entt::entity>(m_pScene->GetPrimaryCameraEntity());
		Camera* camera = GetCamera();
		if (camera == nullptr)
		{
			CORE_LOG_ERROR("Scene has no primary camera: {}.", scene_path);
			return;
		}

		/* 场景画布（HUD）叠在画面上：合成要一张颜色纹理，直出窗口时渲染器会先画到纹理再呈现 */
		camera->GetRenderView()->SetOutputToTexture(m_OutputToTexture);
		camera->GetRenderView()->SetDrawUI(true);
		ApplyViewport();

		/* 场景齐了再启动运行时：它按挂载点逐个建实例并调 OnCreate */
		m_pScene->GetScriptRuntime().Start(*m_pScene);

		/* 多指分路：落在画布控件上的手指喂 UI，另一根留给脚本转视角 */
		DeviceWindow& window = Application::Instance()->GetWindow();
		window.SetUITouchTest([this](const glm::vec2& point) { return HitTestUI(point); });

		CORE_LOG_INFO("Game loaded scene: {} ({}x{}), {} script instance(s).", scene_path,
			window.GetWidth(), window.GetHeight(), m_pScene->GetScriptRuntime().GetInstanceCount());
	}

	void GameLayer::OnDetached()
	{
		/* 先停脚本再丢场景：OnDestroy 里还能安全访问实体 */
		if (m_pScene != nullptr)
			m_pScene->GetScriptRuntime().Stop();

		if (Application* application = Application::Instance())
			application->GetWindow().SetUITouchTest({});
	}

	void GameLayer::OnUpdate(float delta_time)
	{
		PROFILE_CPU_FUNCTION();

		if (GetCamera() == nullptr)
			return;

		/* 输入先进画布：本帧的悬停态脚本当帧就要读 */
		ForwardUIInput();

		m_pScene->OnUpdate(delta_time);

		/* 出错的脚本实例运行时已经停掉，这里只把提示留给宿主显示 */
		ScriptRuntime::Fault fault;
		if (m_pScene->GetScriptRuntime().ConsumeFault(fault))
			m_ScriptFault = fault.ClassName + ": " + fault.Message;

		m_pScene->Render();
	}

	void GameLayer::OnEvent(IEvent* event)
	{
		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<WindowResizeEvent>(BIND_EVENT_FUNC(GameLayer::OnWindowResize));
		dispatcher.Dispatch<KeyPressedEvent>(BIND_EVENT_FUNC(GameLayer::OnKeyPressed));
		dispatcher.Dispatch<KeyReleasedEvent>(BIND_EVENT_FUNC(GameLayer::OnKeyReleased));
		dispatcher.Dispatch<MouseScrolledEvent>(BIND_EVENT_FUNC(GameLayer::OnMouseScrolled));
	}

	Camera* GameLayer::GetCamera() const
	{
		if (m_pScene == nullptr || !m_pScene->IsEntityValid(m_Camera))
			return nullptr;

		const CameraComponent* component = m_pScene->GetRegistry().try_get<CameraComponent>(m_Camera);
		return component != nullptr ? component->m_Camera.get() : nullptr;
	}

	void GameLayer::ApplyViewport()
	{
		Camera* camera = GetCamera();
		if (camera == nullptr)
			return;

		const DeviceWindow& window = Application::Instance()->GetWindow();
		const uint32_t width = window.GetWidth();
		const uint32_t height = window.GetHeight();
		if (width == 0 || height == 0)
			return;

		camera->GetRenderView()->SetViewportRegion({ 0, 0, width, height });
		camera->SetAspectRatio(static_cast<float>(width) / static_cast<float>(height));
	}

	/* 指针是逻辑点，画布按物理像素做命中测试，这里按内容缩放换算 */
	void GameLayer::ForwardUIInput()
	{
		if (!m_pScene->HasUICanvas())
			return;

		UI::UISystem& ui = m_pScene->GetUISystem();

		/* 宿主界面挡着时整段不喂，收掉画布的悬停态；拖拽中照喂，
		 * 指针滑到宿主界面上不该让画布以为手松了 */
		const bool dragging = m_MouseDown[0] || m_MouseDown[1] || m_MouseDown[2];
		if (IsPointerBlocked() && !dragging)
		{
			ui.ProcessMouseLeave();
			return;
		}

		/* 走 UI 那一路指针：移动端是落在控件上的那根手指，桌面与鼠标同源 */
		const DeviceWindow& window = Application::Instance()->GetWindow();
		const glm::vec2 pointer = window.QueryUIPointerPosition() * window.GetContentScale();
		ui.ProcessMouseMove(pointer.x, pointer.y);

		for (int button = 0; button < 3; ++button)
		{
			const bool pressed = window.QueryUIPointerButtonPressed(button);
			if (pressed != m_MouseDown[button])
			{
				if (pressed)
					ui.ProcessMouseButtonDown(button);
				else
					ui.ProcessMouseButtonUp(button);
			}
			m_MouseDown[button] = pressed;
		}
	}

	bool GameLayer::HitTestUI(const glm::vec2& point) const
	{
		if (m_pScene == nullptr)
			return false;

		const float scale = Application::Instance()->GetWindow().GetContentScale();
		return m_pScene->GetUISystem().IsPointOverControl(point.x * scale, point.y * scale);
	}

	bool GameLayer::OnWindowResize(WindowResizeEvent* event)
	{
		(void)event;
		ApplyViewport();
		return false;
	}

	bool GameLayer::OnKeyPressed(KeyPressedEvent* event)
	{
		if (m_pScene == nullptr || !m_pScene->HasUICanvas() || IsKeyboardBlocked())
			return false;

		m_pScene->GetUISystem().ProcessKeyDown(event->GetKeyCode());
		return false;
	}

	bool GameLayer::OnKeyReleased(KeyReleasedEvent* event)
	{
		if (m_pScene == nullptr || !m_pScene->HasUICanvas() || IsKeyboardBlocked())
			return false;

		m_pScene->GetUISystem().ProcessKeyUp(event->GetKeyCode());
		return false;
	}

	bool GameLayer::OnMouseScrolled(MouseScrolledEvent* event)
	{
		if (m_pScene == nullptr || !m_pScene->HasUICanvas() || IsPointerBlocked())
			return false;

		m_pScene->GetUISystem().ProcessMouseWheel(event->GetYOffset());
		return false;
	}
}
