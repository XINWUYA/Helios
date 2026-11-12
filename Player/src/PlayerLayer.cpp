#include "Pch.h"
#include "PlayerLayer.h"

#include "Helios/Application/Application.h"
#include "Helios/Application/ProjectConfig.h"
#include "Helios/Events/ApplicationEvent.h"
#include "Helios/Scene/SceneCommon.h"
#include "Helios/UI/UISystem.h"

namespace Helios
{
	namespace
	{
		/* 项目没写启动场景时的兜底：示例工程里现成的一份 */
		constexpr const char* kFallbackScene = "Scenes/demo_grass.scn";
	}

	PlayerLayer::PlayerLayer() : ILayer("PlayerLayer")
	{
	}

	void PlayerLayer::OnAttached()
	{
		/* 画质开关跟编辑器共用 Project.xml，不然草地与 MSAA 的形态对不上 */
		const ProjectConfig config = LoadProjectConfig();
		ApplyProjectSettings(config);

		m_pScene = CreateSharedPtr<Scene>();

		const std::string scene = config.StartupScene.empty() ? kFallbackScene : config.StartupScene;
		const std::string scene_path = ABSOLUTE_PATH(scene);
		if (!m_pScene->Deserializer(scene_path))
		{
			CORE_LOG_ERROR("Player failed to load scene: {}.", scene_path);
			m_pScene = nullptr;
			return;
		}

		m_Camera = static_cast<entt::entity>(m_pScene->GetPrimaryCameraEntity());
		if (Camera* camera = GetCamera())
		{
			/* 直出默认目标，但要叠场景自己的画布（HUD）
			 * 合成 Pass 需要一张颜色纹理，所以开后置 UI 后走「渲染到纹理再呈现」这条链 */
			camera->GetRenderView()->SetOutputToTexture(false);
			camera->GetRenderView()->SetDrawUI(true);
			ApplyViewport();
		}
		else
		{
			CORE_LOG_ERROR("Player scene has no primary camera: {}.", scene_path);
		}

		/* 多指分路：落在画布控件上的手指喂 UI，另一根留给脚本转视角 */
		Application::Instance()->GetWindow().SetUITouchTest(
			[this](const glm::vec2& point) { return HitTestUI(point); });

		CORE_LOG_INFO("Player loaded scene: {} ({}x{}).", scene_path,
			Application::Instance()->GetWindow().GetWidth(),
			Application::Instance()->GetWindow().GetHeight());
	}

	void PlayerLayer::OnDetached()
	{
		if (Application* application = Application::Instance())
			application->GetWindow().SetUITouchTest({});
	}

	void PlayerLayer::OnUpdate(float delta_time)
	{
		if (m_pScene == nullptr || GetCamera() == nullptr)
			return;

		ForwardUIInput();

		m_pScene->OnUpdate(delta_time);
		m_pScene->Render();
	}

	/* 指针是逻辑点，画布按视口物理像素做命中测试，这里按内容缩放换算 */
	void PlayerLayer::ForwardUIInput()
	{
		if (m_pScene == nullptr || !m_pScene->HasUICanvas())
			return;

		UI::UISystem& ui = m_pScene->GetUISystem();

		/* 走 UI 那一路指针：移动端是落在控件上的那根手指，桌面与鼠标同源 */
		const DeviceWindow& window = Application::Instance()->GetWindow();
		const float scale = window.GetContentScale();
		const glm::vec2 mouse = window.QueryUIPointerPosition() * scale;
		ui.ProcessMouseMove(mouse.x, mouse.y);

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

	/* 触摸落点是不是在画布控件上：是就把它分给 UI，别拿去转视角 */
	bool PlayerLayer::HitTestUI(const glm::vec2& point) const
	{
		if (m_pScene == nullptr)
			return false;

		const float scale = Application::Instance()->GetWindow().GetContentScale();
		return m_pScene->GetUISystem().IsPointOverControl(point.x * scale, point.y * scale);
	}

	void PlayerLayer::OnEvent(IEvent* event)
	{
		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<WindowResizeEvent>(BIND_EVENT_FUNC(PlayerLayer::OnWindowResize));
	}

	bool PlayerLayer::OnWindowResize(WindowResizeEvent* event)
	{
		(void)event;
		ApplyViewport();
		return false;
	}

	Camera* PlayerLayer::GetCamera() const
	{
		if (m_pScene == nullptr || !m_pScene->IsEntityValid(m_Camera))
			return nullptr;

		const CameraComponent* component = m_pScene->GetRegistry().try_get<CameraComponent>(m_Camera);
		return component != nullptr ? component->m_Camera.get() : nullptr;
	}

	void PlayerLayer::ApplyViewport()
	{
		Camera* camera = GetCamera();
		if (camera == nullptr)
			return;

		const auto& window = Application::Instance()->GetWindow();
		const uint32_t width = window.GetWidth();
		const uint32_t height = window.GetHeight();
		if (width == 0 || height == 0)
			return;

		camera->GetRenderView()->SetViewportRegion({ 0, 0, width, height });
		camera->SetAspectRatio(static_cast<float>(width) / static_cast<float>(height));
	}
}
