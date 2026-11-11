#include "Pch.h"
#include "PlayerLayer.h"

#include "Helios/Application/Application.h"
#include "Helios/Application/ProjectConfig.h"
#include "Helios/Events/ApplicationEvent.h"
#include "Helios/Scene/SceneCommon.h"

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
			/* 直出默认目标：没有编辑器，也没有合成 Pass 要接 */
			camera->GetRenderView()->SetOutputToTexture(false);
			ApplyViewport();
		}
		else
		{
			CORE_LOG_ERROR("Player scene has no primary camera: {}.", scene_path);
		}

		CORE_LOG_INFO("Player loaded scene: {} ({}x{}).", scene_path,
			Application::Instance()->GetWindow().GetWidth(),
			Application::Instance()->GetWindow().GetHeight());
	}

	void PlayerLayer::OnUpdate(float delta_time)
	{
		if (m_pScene == nullptr || GetCamera() == nullptr)
			return;

		m_pScene->OnUpdate(delta_time);
		m_pScene->Render();
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
