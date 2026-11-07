#include "SampleScene.h"
#include "SampleCameraController.h"
#include <imgui.h>
#include <Helios/Scene/SceneCommon.h>

namespace Helios
{

SampleSceneLayer::SampleSceneLayer() : ILayer("SampleSceneLayer")
{}

void SampleSceneLayer::OnAttached()
{
	PROFILE_CPU_FUNCTION();

	m_pScene = CreateSharedPtr<Scene>();
	const std::string scene_path = ABSOLUTE_PATH("Scenes/test.scn");
	if (!m_pScene->Deserializer(scene_path))
		CORE_LOG_ERROR("Sample_Scene failed to load {}.", scene_path);

	/* 场景文件里没有相机，这里补一台斜俯视整片草地 */
	auto& window = Application::Instance()->GetWindow();
	const ViewportRegion region{ 0, 0, window.GetWidth(), window.GetHeight() };

	auto camera_entity = m_pScene->CreateEntity("MainCamera");
	auto& camera_component = camera_entity.AddComponent<CameraComponent>();
	camera_component.m_Camera->SetDebugName("MainCamera");
	camera_component.m_Camera->SetNearClip(0.1f);
	camera_component.m_Camera->SetFarClip(200.0f);

	auto render_view = camera_component.m_Camera->GetRenderView();
	render_view->SetViewportRegion(region);
	render_view->SetOwnerScene(m_pScene);

	m_pCameraController = CreateSharedPtr<SampleCameraController>(camera_entity, true);
	m_pCameraController->SetViewportRegion(region);
	m_pCameraController->SetFocalPoint(glm::vec3(0.0f, 1.0f, 2.0f));
	m_pCameraController->SetDistance(20.0f);
	m_pCameraController->SetAngles(0.0f, -25.0f);
}

void SampleSceneLayer::OnUpdate(float delta_time)
{
	PROFILE_CPU_FUNCTION();

	m_pCameraController->OnUpdate(delta_time);
	m_pScene->OnUpdate(delta_time);
	m_pScene->Render();
}

void SampleSceneLayer::OnImGuiRender()
{
	ImGui::Begin("Stats");
	ImGui::Text("Sample Scene: Scenes/test.scn");
	ImGui::Text("- Alt + Mouse Left: Rotate.");
	ImGui::Text("- Alt + Mouse Middle: Pan.");
	ImGui::Text("- Alt + Mouse Right: Zoom.");
	ImGui::End();
}

}
