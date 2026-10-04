#include "SampleIBL.h"
#include "SampleCameraController.h"
#include <imgui.h>
#include <Helios/Scene/SceneCommon.h>
#include <Helios/Scene/ReflectionProbe.h>

namespace Helios
{

SampleIBLLayer::SampleIBLLayer() : ILayer("SampleIBLLayer")
{}

void SampleIBLLayer::OnAttached()
{
	PROFILE_FUNCTION();

	m_pScene = CreateSharedPtr<Scene>();

	/* 向场景中添加SkyBox */
	{
		constexpr TextureLoadConfig load_config{
			.IsFlipV = true,
			.IsGenMips = false,
			.IsHdr = true,
			.IsSrgb = false,
			.SamplerType = SamplerType::Sampler2D,
			.SamplerWrapMode = SamplerWrapMode::ClampToEdge,
			.SamplerMinFilter = SamplerMinFilter::Linear,
			.SamplerMagFilter = SamplerMagFilter::Linear
		};
		m_pSkyTexture = DeviceTexture::Create(ABSOLUTE_PATH("Textures/drakensberg_solitary_mountain_4k.hdr"), load_config);

		RenderRasterState raster_state;
		raster_state.EnableDepthWrite = true;
		raster_state.DepthCompareFunc = CompareFunc::GreaterEqual;
		raster_state.CullMode = CullMode::Cull_Front;

		const auto shader = ShaderAssetManager::Instance().GetOrLoad(ABSOLUTE_PATH("Shaders/SkyBox.glsl"));
		auto material = Material::Create(shader);
		material->SetParameters(ParamType::Texture, "u_SkyTex", m_pSkyTexture);
		material->SetRasterState(raster_state);

		auto entity = m_pScene->CreateEntity("SkyBox");
		auto& model_component = entity.AddComponent<ModelComponent>();
		model_component.m_Model = Model::Create(BuiltinModelType::Sphere, material);
		model_component.m_Model->SetDebugName("SkyBox");
	}

	/* 向场景中添加反射探针 */
	{
		auto entity = m_pScene->CreateEntity("ReflectionProbe");
		auto& probe_component = entity.AddComponent<ReflectionProbeComponent>();
		probe_component.m_ReflectionProbe->SetSkyBoxTexture(m_pSkyTexture);
        probe_component.m_ReflectionProbe->SetRealtime(false);
        probe_component.m_ReflectionProbe->SetDebugName("ReflectionProbe");
		m_pReflectionProbeComponent = &probe_component;
	}

	/* 添加PBR球体 */
	{
		const auto shader = ShaderAssetManager::Instance().GetOrLoad(ABSOLUTE_PATH("Shaders/ReflectionProbe/IBLLit.glsl"));
		m_pPBRMaterial = Material::Create(shader);
		m_pPBRMaterial->SetTexture("u_AlbedoTexture", DeviceTexture::White());
		m_pPBRMaterial->SetTexture("u_RoughnessTexture", DeviceTexture::CreateWithSolidColor(glm::vec3(0.15f)));
		m_pPBRMaterial->SetTexture("u_MetalnessTexture", DeviceTexture::CreateWithSolidColor(glm::vec3(1.0f)));
		m_pPBRMaterial->SetTexture("u_NormalTexture", DeviceTexture::Normal());
		m_pPBRMaterial->SetParameters(ParamType::Float, "u_EnvironmentStrength", 1.0f);

		auto entity = m_pScene->CreateEntity("PBRSphere");
		auto& model_component = entity.AddComponent<ModelComponent>();
		model_component.m_Model = Model::Create(BuiltinModelType::Sphere, m_pPBRMaterial);
		model_component.m_Model->SetDebugName("PBRSphere");

		auto& transform = entity.GetComponent<TransformComponent>();
		transform.m_Scale = glm::vec3(2.0f);
	}

	/* 向场景中添加方向光 */
	{
		auto light_entity = m_pScene->CreateEntity("DirectionalLight");
		auto& light_component = light_entity.AddComponent<LightComponent>(LightType::Directional);
		m_pDirectionLight = StaticPtrCast<DirectionalLight>(light_component.m_Light);
		m_pDirectionLight->SetDirection(glm::normalize(glm::vec3(0.5f, -0.7f, -0.5f)));
		m_pDirectionLight->SetIntensity(2.0f);
		m_pDirectionLight->SetDebugName("DirectionalLight");

		/* 配置阴影 */
		m_pDirectionLight->SetIsCastShadow(true);
		auto shadow_map_info = CreateSharedPtr<ShadowMapInfo>();
		shadow_map_info->Size = 2048;
		shadow_map_info->CascadeCnt = 1;
		/* 见 Sample_CSM：CascadeRadius 只是"最小半边长"下限，0 表示完全 tight-fit */
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
		camera_component.m_Camera->SetNearClip(0.1f);
		camera_component.m_Camera->SetFarClip(100.0f);

		m_pCameraController = CreateSharedPtr<SampleCameraController>(camera_entity);
		m_pCameraController->SetViewportRegion({ 0, 0, window.GetWidth(), window.GetHeight() });
		m_pCameraController->SetFocus(true);
		m_pCameraController->SetDistance(10.0f);
		m_pCameraController->SetFocalPoint(glm::vec3(0.0f, 0.0f, 0.0f));

		auto render_view = camera_component.m_Camera->GetRenderView();
		render_view->SetViewportRegion({ 0, 0, window.GetWidth(), window.GetHeight() });
		render_view->SetOwnerScene(m_pScene);
	}
}

void SampleIBLLayer::OnDetached()
{
	ILayer::OnDetached();
}

void SampleIBLLayer::OnUpdate(float delta_time)
{
	PROFILE_FUNCTION();

	m_pCameraController->OnUpdate(delta_time);
	m_pScene->OnUpdate(delta_time);
	m_pScene->Render();
}

void SampleIBLLayer::OnImGuiRender()
{
	PROFILE_FUNCTION();

	ImGui::Begin("Stats");
	ImGui::Text("Sample IBL:");
	ImGui::Text("- Alt + Mouse Left: Rotate.");

	static float env_strength = 1.0f;
	if (ImGui::SliderFloat("Env Strength", &env_strength, 0.0f, 3.0f))
	{
		if (m_pPBRMaterial)
			m_pPBRMaterial->SetParameters(ParamType::Float, "u_EnvironmentStrength", env_strength);
	}

	if (auto* probe = m_pReflectionProbeComponent)
	{
		static bool bake_from_scene = false;
		if (ImGui::Checkbox("Bake From Scene", &bake_from_scene))
		{
			probe->m_ReflectionProbe->SetSkyBoxTexture(bake_from_scene ? nullptr : m_pSkyTexture);
            probe->m_ReflectionProbe->Reset();
        }

		bool realtime_bake = probe->m_ReflectionProbe->IsRealtime();
		if (ImGui::Checkbox("Realtime Bake", &realtime_bake))
		{
			probe->m_ReflectionProbe->SetRealtime(realtime_bake);
			probe->m_ReflectionProbe->Reset();
        }
	}

	ImGui::Separator();
	ImGui::Text("Light Dir");
	auto light_dir = m_pDirectionLight->GetDirection();
	if (ImGuiExt::DrawDirectionIndicator("Light Dir", light_dir, 0))
		m_pDirectionLight->SetDirection(light_dir);
	ImGui::End();
}

}
