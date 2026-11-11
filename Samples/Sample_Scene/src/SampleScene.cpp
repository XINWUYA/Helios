#include "SampleScene.h"

#include <imgui.h>

#include "Helios/Application/ProjectConfig.h"
#include "Helios/Events/ApplicationEvent.h"
#include "Helios/Events/KeyEvent.h"
#include "Helios/Events/MouseEvent.h"
#include "Helios/Scene/GrassField.h"
#include "Helios/Scene/SceneCommon.h"
#include "Helios/Script/ScriptRuntime.h"
#include "Helios/UI/UISystem.h"

namespace Helios
{
	namespace
	{
		constexpr const char* kScenePath = "Scenes/demo_grass.scn";

		/* 画质开关跟编辑器共用一份项目配置（Project.xml）。不接的话草地退回
		 * 按地块逐次提交，几十个 dc，跟 Play 的画面和开销都对不上 */
		void ApplyProjectRendering()
		{
			const ProjectConfig config = LoadProjectConfig();
			ApplyProjectSettings(config);

			CORE_LOG_INFO("[Sample] MSAA={} grass indirect={} placement={} density={}.",
				config.Rendering.MSAA, GrassField::IsGlobalIndirectDrawEnabled(),
				GrassField::IsGlobalGpuPlacementEnabled(), GrassField::GetGlobalDensityScale());
		}
	}

	SampleSceneLayer::SampleSceneLayer() : ILayer("SampleSceneLayer")
	{}

	SampleSceneLayer::~SampleSceneLayer()
	{
		/* 先停脚本再丢场景：OnDestroy 里还能安全访问实体 */
		if (m_pScene != nullptr)
			m_pScene->GetScriptRuntime().Stop();
	}

	void SampleSceneLayer::OnAttached()
	{
		PROFILE_CPU_FUNCTION();

		/* 脚本先挂上：场景里的挂载点按类名建实例，类得先在注册表里 */
		m_Scripts.LoadLatest();
		ApplyProjectRendering();

		m_pScene = CreateSharedPtr<Scene>();
		const std::string scene_path = ABSOLUTE_PATH(kScenePath);
		if (!m_pScene->Deserializer(scene_path))
		{
			CORE_LOG_ERROR("Sample_Scene failed to load {}.", scene_path);
			m_pScene = nullptr;
			return;
		}

		m_Camera = static_cast<entt::entity>(m_pScene->GetPrimaryCameraEntity());
		if (GetCamera() == nullptr)
		{
			CORE_LOG_ERROR("{} has no primary camera.", scene_path);
			return;
		}

		/* 画面进视图纹理：画布要叠在最终画面上，合成 Pass 只在纹理出口上有接线
		 * （编辑器 Game 视图同样按时序取这张纹理显示） */
		GetCamera()->GetRenderView()->SetOutputToTexture(true);
		/* 画布叠在游戏画面上：编辑器里只有 Game 视图开这个开关，示例的视图就是游戏画面 */
		GetCamera()->GetRenderView()->SetDrawUI(true);
		ApplyViewport();

		m_pScene->GetScriptRuntime().Start(*m_pScene);
	}

	void SampleSceneLayer::OnUpdate(float delta_time)
	{
		PROFILE_CPU_FUNCTION();

		if (GetCamera() == nullptr)
			return;

		/* 游戏输入的闸，与编辑器 Game 视图同一道：ImGui 拿着指针或键盘时关掉，
		 * 脚本读到的按键与指针一律为空，拖面板不会顺带转视角 */
		const ImGuiIO& io = ImGui::GetIO();
		Input::SetGameInput(!io.WantCaptureMouse && !io.WantCaptureKeyboard,
			InputViewport{ glm::vec2(0.0f), glm::vec2(io.DisplaySize.x, io.DisplaySize.y) });

		/* 输入先进画布：本帧的悬停态脚本当帧就要读 */
		ForwardUIInput();

		m_pScene->OnUpdate(delta_time);

		/* 脚本抛出的异常运行时已经停掉那个实例，这里只把提示留下来给面板看 */
		ScriptRuntime::Fault fault;
		if (m_pScene->GetScriptRuntime().ConsumeFault(fault))
			m_Fault = fault.ClassName + ": " + fault.Message;

		m_pScene->Render();
	}

	void SampleSceneLayer::OnEvent(IEvent* event)
	{
		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<WindowResizeEvent>(BIND_EVENT_FUNC(SampleSceneLayer::OnWindowResize));
		dispatcher.Dispatch<KeyPressedEvent>(BIND_EVENT_FUNC(SampleSceneLayer::OnKeyPressed));
		dispatcher.Dispatch<KeyReleasedEvent>(BIND_EVENT_FUNC(SampleSceneLayer::OnKeyReleased));
		dispatcher.Dispatch<MouseScrolledEvent>(BIND_EVENT_FUNC(SampleSceneLayer::OnMouseScrolled));
	}

	void SampleSceneLayer::OnImGuiRender()
	{
		/* 视图纹理铺满整窗，面板叠在它上面。纹理行序与 ImGui 上下相反，UV 要翻一下 */
		if (Camera* camera = GetCamera())
		{
			if (const SharedPtr<DeviceTexture> texture = camera->GetRenderView()->GetRenderTarget())
			{
				ImGui::GetBackgroundDrawList()->AddImage((ImTextureID)texture.get(),
					ImVec2(0.0f, 0.0f), ImGui::GetIO().DisplaySize, ImVec2{ 0, 1 }, ImVec2{ 1, 0 });
			}
		}

		ImGui::Begin("Stats");
		ImGui::Text("Scene: %s", kScenePath);
		ImGui::Text("Scripts: %s", m_Scripts.GetStatus().c_str());
		ImGui::Text("- Drag the joystick to move.");
		ImGui::Text("- Drag anywhere else to look around.");
		if (!m_Fault.empty())
			ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "Script error: %s", m_Fault.c_str());
		ImGui::End();
	}

	Camera* SampleSceneLayer::GetCamera() const
	{
		if (m_pScene == nullptr || !m_pScene->IsEntityValid(m_Camera))
			return nullptr;

		const CameraComponent* component = m_pScene->GetRegistry().try_get<CameraComponent>(m_Camera);
		return component != nullptr ? component->m_Camera.get() : nullptr;
	}

	void SampleSceneLayer::ApplyViewport()
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

	/* 光标是逻辑点，画布按视口物理像素做命中测试，这里按内容缩放换算 */
	void SampleSceneLayer::ForwardUIInput()
	{
		if (!m_pScene->HasUICanvas())
			return;

		UI::UISystem& ui = m_pScene->GetUISystem();

		/* 面板盖住指针时整段不喂，画布上的悬停态收掉；拖拽中照喂，
		 * 指针滑到面板上不该让画布以为手松了 */
		const bool dragging = m_MouseDown[0] || m_MouseDown[1] || m_MouseDown[2];
		if (ImGui::GetIO().WantCaptureMouse && !dragging)
		{
			ui.ProcessMouseLeave();
			return;
		}

		const float scale = Application::Instance()->GetWindow().GetContentScale();
		const glm::vec2 mouse = Input::GetMousePosRaw() * scale;
		ui.ProcessMouseMove(mouse.x, mouse.y);

		for (int button = 0; button < 3; ++button)
		{
			const bool pressed = Input::IsMouseButtonPressedRaw(static_cast<MouseCode>(button));
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

	bool SampleSceneLayer::OnWindowResize(WindowResizeEvent*)
	{
		ApplyViewport();
		return false;
	}

	bool SampleSceneLayer::OnKeyPressed(KeyPressedEvent* event)
	{
		if (m_pScene == nullptr || !m_pScene->HasUICanvas() || ImGui::GetIO().WantCaptureKeyboard)
			return false;

		m_pScene->GetUISystem().ProcessKeyDown(event->GetKeyCode());
		return false;
	}

	bool SampleSceneLayer::OnKeyReleased(KeyReleasedEvent* event)
	{
		if (m_pScene == nullptr || !m_pScene->HasUICanvas() || ImGui::GetIO().WantCaptureKeyboard)
			return false;

		m_pScene->GetUISystem().ProcessKeyUp(event->GetKeyCode());
		return false;
	}

	bool SampleSceneLayer::OnMouseScrolled(MouseScrolledEvent* event)
	{
		if (m_pScene == nullptr || !m_pScene->HasUICanvas() || ImGui::GetIO().WantCaptureMouse)
			return false;

		m_pScene->GetUISystem().ProcessMouseWheel(event->GetYOffset());
		return false;
	}
}
