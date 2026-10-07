#include "SampleCSM.h"
#include <imgui.h>
#include <Helios/Scene/Material.h>
#include "SampleCameraController.h"

namespace Helios
{

void SampleCSM::OnAttached()
{
	m_pScene = CreateSharedPtr<Scene>();

	/* 向场景中添加SkyBox */
	{
		constexpr TextureLoadConfig load_config{
			true,
			false,
			true,
			false,
			SamplerType::Sampler2D,
			SamplerWrapMode::ClampToEdge,
			SamplerMinFilter::Linear,
			SamplerMagFilter::Linear
		};
		const auto sky_texture = DeviceTexture::Create(ABSOLUTE_PATH("Textures/drakensberg_solitary_mountain_4k.hdr"), load_config);

		RenderRasterState raster_state;
		raster_state.EnableDepthWrite = true;
		raster_state.DepthCompareFunc = CompareFunc::GreaterEqual;
		raster_state.CullMode = CullMode::Cull_None;

		const auto shader = ShaderAssetManager::Instance().GetOrLoad(ABSOLUTE_PATH("Shaders/SkyBox.glsl"));
		const auto material = Material::Create(shader);
		material->SetParameters(ParamType::Texture, "u_SkyTex", sky_texture);
		material->SetRasterState(raster_state);

		auto entity = m_pScene->CreateEntity("SkyBox");
		auto& model_component = entity.AddComponent<ModelComponent>();
		model_component.m_Model = Model::Create(BuiltinModelType::Sphere, material);
		model_component.m_Model->SetDebugName("SkyBox");
	}

	/* 向场景中添加地面 */
	{
		auto entity = m_pScene->CreateEntity("Plane");
		auto& model_component = entity.AddComponent<ModelComponent>();

		const auto shader = ShaderAssetManager::Instance().GetOrLoad(ABSOLUTE_PATH("Shaders/ForwardShaders/BuiltinLit.glsl"));
		auto material = Material::Create(shader);
		auto texture = TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH("Textures/Grass2.png"));
		material->SetTexture("u_AlbedoTexture", texture);
		material->SetParameters(ParamType::Vec4, "u_AlbedoTilingOffset", glm::vec4(4, 4, 0, 0));
		model_component.m_Model = Model::Create(BuiltinModelType::Plane, material);
		model_component.m_Model->SetDebugName("Plane");

		auto& transform_component = entity.GetComponent<TransformComponent>();
		transform_component.m_Scale = glm::vec3(15.0f, 1.0f, 15.0f);
	}

	/* 向场景中添加房屋模型 */
	glm::vec3 center_pos = glm::vec3(0.0f);
	{
		auto entity = m_pScene->CreateEntity("Cottage");
		auto& model_component = entity.AddComponent<ModelComponent>();
		model_component.m_Model = Model::Create(ABSOLUTE_PATH("Models/big_cottage/Cottage.mesh"));
		model_component.m_Model->SetDebugName("Cottage");
		auto& transform_component = entity.GetComponent<TransformComponent>();
		transform_component.m_Scale = glm::vec3(0.6f);
		transform_component.m_Position = glm::vec3(0.0f, 0.0f, 0.0f);
		center_pos = (model_component.m_Model->GetAABBMin() + model_component.m_Model->GetAABBMax()) * 0.5f;
	}

	/* 向场景中添加方向光 */
	{
		auto directional_light_entity = m_pScene->CreateEntity("DirectionalLight");
		auto& light_component = directional_light_entity.AddComponent<LightComponent>(LightType::Directional);
		m_pDirectionLight = StaticPtrCast<DirectionalLight>(light_component.m_Light);
		m_pDirectionLight->SetDirection(glm::normalize(glm::vec3(0.5f, -0.7f, -0.5f)));
		m_pDirectionLight->SetIntensity(2.0f);
		m_pDirectionLight->SetDebugName("DirectionalLight");

		/* 配置阴影 */
		m_pDirectionLight->SetIsCastShadow(true);
		auto shadow_map_info = CreateSharedPtr<ShadowMapInfo>();
		shadow_map_info->Size = 2048;
		shadow_map_info->CascadeCnt = 4;
		/* CascadeRadius 是各级联"最小半边长"下限，用于稳住近处级联的分辨率；
		 * 填 0 表示完全按几何体 tight-fit。 */
		shadow_map_info->CascadeRadius = glm::vec4(0.0f);
		shadow_map_info->ConstantBias = 0.01f;
		shadow_map_info->NormalBias = 1.0f;
		shadow_map_info->ShadowFar = 200.0f;
		m_pDirectionLight->SetShadowMapInfo(shadow_map_info);
	}

	auto& window = Application::Instance()->GetWindow();

	/* 向场景中添加相机 */
	{
		auto camera_entity = m_pScene->CreateEntity("MainCamera");
		auto& camera_component = camera_entity.AddComponent<CameraComponent>();
		camera_component.m_Camera->SetDebugName("MainCamera");
		camera_component.m_Camera->SetNearClip(1.0f);
		camera_component.m_Camera->SetFarClip(100.0f);

		m_pCameraController = CreateSharedPtr<SampleCameraController>(camera_entity);
		m_pCameraController->SetViewportRegion({ 0, 0, window.GetWidth(), window.GetHeight() });
		m_pCameraController->SetFocus(true);
		m_pCameraController->SetDistance(15.0f);
		m_pCameraController->SetFocalPoint(center_pos);

		auto render_view = camera_component.m_Camera->GetRenderView();
		render_view->SetViewportRegion({ 0, 0, window.GetWidth(), window.GetHeight() });
		render_view->SetOwnerScene(m_pScene);
	}
}

void SampleCSM::OnDetached()
{
	ILayer::OnDetached();
}

void SampleCSM::OnUpdate(float delta_time)
{
	PROFILE_FUNCTION();

	m_pCameraController->OnUpdate(delta_time);

	m_pScene->OnUpdate(delta_time);
	m_pScene->Render();
}

void SampleCSM::OnImGuiRender()
{
	PROFILE_FUNCTION();

	ImGui::Begin("Stats");
	ImGui::Text("Sample CSM:");
	ImGui::Text("- Alt + Mouse Left: Rotate.");

	auto light_dir = m_pDirectionLight->GetDirection();
	if (ImGuiExt::DrawDirectionIndicator("Light Dir", light_dir, 0))
		m_pDirectionLight->SetDirection(light_dir);
	ImGui::End();
}
}