#include "SampleSkyBox.h"
#include <imgui.h>
#include <Helios/Scene/Material.h>
#include "SampleCameraController.h"

namespace Helios
{

void SampleSkyBox::OnAttached()
{
	m_pScene = CreateSharedPtr<Scene>();
	
	/* 向场景中添加SkyBox */
	{
		TextureLoadConfig load_config{
			.IsFlipV = true,
			.IsGenMips = false,
			.IsHdr = true,
			.IsSrgb = false,
			.SamplerType = SamplerType::Sampler2D,
			.SamplerWrapMode = SamplerWrapMode::ClampToEdge,
			.SamplerMinFilter = SamplerMinFilter::Linear,
			.SamplerMagFilter = SamplerMagFilter::Linear
		};
		auto sky_texture = DeviceTexture::Create(ABSOLUTE_PATH("Textures/drakensberg_solitary_mountain_4k.hdr"), load_config);

		RenderRasterState raster_state;
		raster_state.EnableDepthWrite = true;
		raster_state.DepthCompareFunc = CompareFunc::GreaterEqual;
		raster_state.CullMode = CullMode::Cull_Front;

		auto shader = ShaderAssetManager::Instance().GetOrLoad(ABSOLUTE_PATH("Shaders/SkyBox.glsl"));
		auto material = Material::Create(shader);
		material->SetParameters(ParamType::Texture, "u_SkyTex", sky_texture);
		material->SetRasterState(raster_state);

		auto skybox = Model::Create(BuiltinModelType::Sphere, material);

		auto model_entity = m_pScene->CreateEntity("SkyBox");
		auto& model_component = model_entity.AddComponent<ModelComponent>();
		model_component.m_Model = skybox;
	}

	auto& window = Application::Instance()->GetWindow();

	/* 向场景中添加相机 */
	{
		auto camera_entity = m_pScene->CreateEntity("MainCamera");
		auto& camera_component = camera_entity.AddComponent<CameraComponent>();
		m_pCameraController = CreateSharedPtr<SampleCameraController>(camera_entity);
		m_pCameraController->SetFocus(true);
		m_pCameraController->SetViewportRegion({ 0, 0, window.GetWidth(), window.GetHeight() });

		auto render_view = camera_component.m_Camera->GetRenderView();
		render_view->SetViewportRegion({ 0, 0, window.GetWidth(), window.GetHeight() });
		render_view->SetOwnerScene(m_pScene);
	}
}

void SampleSkyBox::OnDetached()
{
	ILayer::OnDetached();
}

void SampleSkyBox::OnUpdate(float delta_time)
{
	Renderer::SetClearColor(glm::vec4(0.2f, 0.3f, 0.3f, 1.0f));
	Renderer::Clear();

	m_pCameraController->OnUpdate(delta_time);

	m_pScene->OnUpdate(delta_time);
	m_pScene->Render();
}

void SampleSkyBox::OnImGuiRender()
{
	ImGui::Begin("Stats");
	ImGui::Text("Sample SkyBox: Show SkyBox.");
	ImGui::Text("- Alt + Mouse Left: Rotate.");
	ImGui::End();
}
}