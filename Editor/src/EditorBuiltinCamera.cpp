#include "Pch.h"
#include "EditorBuiltinCamera.h"
#include "SceneEditor/SceneGizmos.h"
#include <glm/gtx/quaternion.hpp>
#include <cmath>

#include "Helios/Application/AssetManager.h"
#include "Helios/Common/Math.h"
#include "Helios/Common/Utils.h"
#include "Helios/Scene/Material.h"
#include "Helios/Scene/ReflectionProbe.h"
#include "Helios/Scene/Scene.h"
#include "Helios/Scene/ShadowMap.h"
#include "Helios/VirtualDevice/DeviceTexture.h"

namespace Helios
{
	namespace
	{
		/* 延迟光照一次参与"逐像素最近探针"选择的探针上限：受纹理 / 采样器槽位
		 * 预算约束（lighting.glsl 里每个探针占一组固定绑定点）。
		 * 场景内探针多于上限时取离相机最近的若干个（见 CollectClosestBakedProbes）。 */
		constexpr size_t kMaxDeferredIBLProbes = 3;

		/* G-Buffer 采样点缺省时补的默认贴图：法线图按名字认，亮度类（Albedo / Roughness）用白，其余用黑。 */
		SharedPtr<DeviceTexture> DefaultTextureForSampler(const std::string& sampler_name)
		{
			if (sampler_name.find("Normal") != std::string::npos)
				return TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH("Textures/normal.png"));
			if (sampler_name.find("Albedo") != std::string::npos || sampler_name.find("Roughness") != std::string::npos)
				return TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH("Textures/White.png"));
			return TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH("Textures/Black.png"));
		}

		SharedPtr<Material> CreateGBufferMaterial(
			const SharedPtr<Material>& source, const SharedPtr<DeviceShader>& gbuffer_shader)
		{
			auto material = Material::Create(gbuffer_shader);
			for (const auto& [_, parameter] : source->GetAllParameters())
			{
				if (parameter.Type == ParamType::Texture)
				{
					if (gbuffer_shader->GetUniformBinding(parameter.Name) < 0)
						continue;
					const auto texture = std::any_cast<std::pair<SharedPtr<DeviceTexture>, uint32_t>>(parameter.Value);
					material->SetTexture(parameter.Name, texture.first);
				}
				else
				{
					material->SetParameters(parameter.Type, parameter.Name, parameter.Value);
				}
			}

			/* 源材质没覆盖到的采样点补默认贴图：default.glsl 的采样点必须全部有绑定，
			 * 缺一张就是未定义输入（Metal 校验层会直接断言）。参数不全的老材质、新建材质
			 * 都靠这一步在编辑器里兜住 —— 直接遍历目标 shader 的反射列表，与 shader 声明保持同步。 */
			for (const auto& sampler_entry : gbuffer_shader->GetReflectionData().SamplerBindings)
			{
				const std::string& sampler_name = sampler_entry.first;
				if (material->GetAllParameters().find(ToID(sampler_name)) != material->GetAllParameters().end())
					continue;
				material->SetTexture(sampler_name, DefaultTextureForSampler(sampler_name));
			}

			material->SetRasterState(source->GetRasterState());
			return material;
		}

		/* === 视口导航参数 === */
		constexpr float kMouseSensitivity = 0.003f;	/* 像素 → 拖拽量的统一系数（沿用原手感） */
		constexpr float kMinDistance = 0.5f;		/* 轨道距离上下限 */
		constexpr float kMaxDistance = 1000.0f;
		constexpr float kMaxPitch = 1.553f;			/* 俯仰限位 ±89°：翻越天顶后视线会翻转 */
		constexpr float kWheelZoomStep = 0.9f;		/* 滚轮每格缩放比（乘性） */
		constexpr float kWheelSpeedStep = 1.15f;	/* 飞行速度每格调整比（乘性） */
		constexpr float kMinFlySpeed = 0.5f;
		constexpr float kMaxFlySpeed = 100.0f;
	}

	EditorCamera::EditorCamera(float fov, float aspect_ratio, float near_clip, float far_clip)
		: Camera(CameraProjectionType::Perspective, fov, aspect_ratio, near_clip, far_clip)
	{
		PROFILE_FUNCTION();

		UpdateCameraDirections();

		UpdateProjectionMatrix();
		UpdateViewMatrix();
	}

	EditorCamera::~EditorCamera()
	{
	}

	void EditorCamera::OnUpdate(float delta_time)
	{
		PROFILE_FUNCTION();

		Camera::OnUpdate(delta_time);

		UpdateNavigation(delta_time, m_IsNavigationAllowed);

		UpdateViewMatrix();
	}

	/* 视口导航状态机：手势从"指针在视口上时按下按键"起手（allow_new_gesture 由场景层每帧
	 * 授权）；起手后就锁定到那次按键松开 —— 拖拽中指针移出视口、松开修饰键，都不中断。 */
	void EditorCamera::UpdateNavigation(float delta_time, bool allow_new_gesture)
	{
		PROFILE_FUNCTION();

		const bool is_alt_pressed = Input::IsKeyPressed(Key::LeftAlt) || Input::IsKeyPressed(Key::RightAlt);
		const bool is_lmb_pressed = Input::IsMouseButtonPressed(Mouse::ButtonLeft);
		const bool is_mmb_pressed = Input::IsMouseButtonPressed(Mouse::ButtonMiddle);
		const bool is_rmb_pressed = Input::IsMouseButtonPressed(Mouse::ButtonRight);

		/* 手势保持条件：其按键仍按住（Orbit / ZoomDrag 还要求 Alt —— 飞行中按下 Alt 会顺势切换为推拉） */
		bool is_gesture_held = false;
		switch (m_ActiveGesture)
		{
		case NavGesture::Orbit:		is_gesture_held = is_alt_pressed && is_lmb_pressed; break;
		case NavGesture::Pan:		is_gesture_held = is_mmb_pressed; break;
		case NavGesture::ZoomDrag:	is_gesture_held = is_alt_pressed && is_rmb_pressed; break;
		case NavGesture::Fly:		is_gesture_held = is_rmb_pressed && !is_alt_pressed; break;
		default:					break;
		}

		if (m_ActiveGesture != NavGesture::None && !is_gesture_held)
			m_ActiveGesture = NavGesture::None;

		if (m_ActiveGesture == NavGesture::None && allow_new_gesture)
		{
			/* 起手要求按键“按下沿”：按键从别的窗口拖进视口、或按住不放滑入视口，
			 * 都不算起手 —— 否则在视口外拖拽时划过视口会突然开始转视角 */
			const bool is_alt_edge = is_alt_pressed && !m_WasAltPressed;
			const bool is_lmb_edge = is_lmb_pressed && !m_WasLmbPressed;
			const bool is_mmb_edge = is_mmb_pressed && !m_WasMmbPressed;
			const bool is_rmb_edge = is_rmb_pressed && !m_WasRmbPressed;

			if (is_alt_pressed)
			{
				if (is_lmb_pressed && is_lmb_edge)
					m_ActiveGesture = NavGesture::Orbit;
				else if (is_mmb_pressed && is_mmb_edge)
					m_ActiveGesture = NavGesture::Pan;
				else if (is_rmb_pressed && (is_rmb_edge || is_alt_edge))
					m_ActiveGesture = NavGesture::ZoomDrag; /* 飞行中按下 Alt：顺势切换为推拉 */
			}
			else if (is_rmb_pressed && is_rmb_edge)
			{
				m_ActiveGesture = NavGesture::Fly;
			}
			else if (is_mmb_pressed && is_mmb_edge)
			{
				m_ActiveGesture = NavGesture::Pan;
			}
		}

		/* 鼠标增量逐帧维护（闲置时也更新）：起手帧的增量只是正常的单帧位移，不会跳视角 */
		const glm::vec2 mouse_pos = Input::GetMousePos();
		const glm::vec2 mouse_delta = (mouse_pos - m_LastMousePosition) * kMouseSensitivity;
		m_LastMousePosition = mouse_pos;

		switch (m_ActiveGesture)
		{
		case NavGesture::Orbit:		OnMouseRotate(mouse_delta); break;
		case NavGesture::Pan:		OnMousePan(mouse_delta); break;
		case NavGesture::ZoomDrag:	OnMouseZoom(mouse_delta.y); break;
		case NavGesture::Fly:		UpdateFly(mouse_delta, delta_time); break;
		default:					break;
		}

		/* 帧末记录按键状态（下一帧的“按下沿”判据） */
		m_WasAltPressed = is_alt_pressed;
		m_WasLmbPressed = is_lmb_pressed;
		m_WasMmbPressed = is_mmb_pressed;
		m_WasRmbPressed = is_rmb_pressed;
	}

	/* 飞行（RMB 按住）：鼠标原地转向；WASD 沿视线、E/Q 沿世界竖直方向平移。
	 * 转向时枢轴重新落在视线正前方同距离处 —— 眼位保持不动，退出飞行后
	 * 轨道中心就是当时的视线落点，无需交接。 */
	void EditorCamera::UpdateFly(const glm::vec2& mouse_delta, float delta_time)
	{
		PROFILE_FUNCTION();

		if (mouse_delta.x != 0.0f || mouse_delta.y != 0.0f)
		{
			/* 转向前的眼位（位置是派生量，这里直接用轨道参数求出） */
			const glm::vec3 eye_position = m_FocalPoint - m_ForwardDirection * m_Distance;

			m_Yaw += mouse_delta.x * RotateSpeed();
			m_Pitch = glm::clamp(m_Pitch + mouse_delta.y * RotateSpeed(), -kMaxPitch, kMaxPitch);
			UpdateCameraDirections();
			m_FocalPoint = eye_position + m_ForwardDirection * m_Distance;

			m_IsDirty = true;
		}

		glm::vec3 move_direction(0.0f);
		if (Input::IsKeyPressed(Key::W))
			move_direction += m_ForwardDirection;
		if (Input::IsKeyPressed(Key::S))
			move_direction -= m_ForwardDirection;
		if (Input::IsKeyPressed(Key::D))
			move_direction += m_RightDirection;
		if (Input::IsKeyPressed(Key::A))
			move_direction -= m_RightDirection;
		/* 竖直用世界轴：俯仰朝下时 Q/E 不会变成前冲 */
		if (Input::IsKeyPressed(Key::E))
			move_direction += glm::vec3(0.0f, 1.0f, 0.0f);
		if (Input::IsKeyPressed(Key::Q))
			move_direction -= glm::vec3(0.0f, 1.0f, 0.0f);

		if (glm::dot(move_direction, move_direction) > 0.0f)
		{
			/* 眼位与枢轴同步平移（斜向先归一化，避免比单键更快） */
			m_FocalPoint += glm::normalize(move_direction) * (m_MoveSpeed * delta_time);
			m_IsDirty = true;
		}
	}

	/* 设置视口区域 */
	void EditorCamera::SetViewportRegion(const ViewportRegion& region)
	{
		PROFILE_FUNCTION();

		m_ViewportRegion = region;
		m_pRenderView->SetViewportRegion(region);

		m_AspectRatio = static_cast<float>(m_ViewportRegion.Width) / static_cast<float>(m_ViewportRegion.Height);
		UpdateProjectionMatrix();
	}

	/* 构建内置的FrameGraph（延迟渲染：GBuffer + Lighting）
	 * 由 RenderView 在执行时回调，故渲染图每帧按当前视口尺寸重建。 */
	bool EditorCamera::ConstructRenderView(RenderView& render_view)
	{
		PROFILE_FUNCTION();

		/* 视口尺寸无效时不组织渲染图：这里必须返回 true（表示"本相机负责组织"），
		 * 否则会回落到默认前向图并沿用 0 尺寸的渲染目标。渲染图为空时输出为空。 */
		if (m_ViewportRegion.Width <= 0 || m_ViewportRegion.Height <= 0)
			return true;

		auto& frame_graph = render_view.GetFrameGraph();
		frame_graph->Reset();

		FrameGraphTexture::Descriptor color_target_desc;
		color_target_desc.Width = m_ViewportRegion.Width;
		color_target_desc.Height = m_ViewportRegion.Height;
		color_target_desc.TextureFormat = TextureFormat::RGBA8;

		/* 世界位置通道用浮点格式：RGBA8 会把世界坐标钳制到 [0,1]（负值归零、
		 * 大于 1 截平），光照阶段拿它做级联阴影变换时采样点全部落在错误位置 ——
		 * 阴影与场景对不上（偏移、截断，甚至全域误判为受光/被遮）。 */
		FrameGraphTexture::Descriptor world_pos_target_desc = color_target_desc;
		world_pos_target_desc.TextureFormat = TextureFormat::RGBA32F;

		FrameGraphTexture::Descriptor depth_target_desc;
		depth_target_desc.Width = m_ViewportRegion.Width;
		depth_target_desc.Height = m_ViewportRegion.Height;
		depth_target_desc.TextureFormat = TextureFormat::Depth32;

		FrameGraphTexture::Descriptor object_id_desc;
		object_id_desc.Width = m_ViewportRegion.Width;
		object_id_desc.Height = m_ViewportRegion.Height;
		object_id_desc.TextureFormat = TextureFormat::R32I;

		/* GBuffer Pass 使用 default.glsl；普通材质在提交前转换为该 shader 对应的临时材质。 */
		const auto gbuffer_shader = ShaderAssetManager::Instance().GetOrLoad(
			ABSOLUTE_PATH("Shaders/default.glsl"));

		/* 阴影：先生成中性深度数组（保证光照着色器总能采样到合法纹理），
		 * 本视图有投影光源（级联 / 点光 / 聚光）时再生成真实阴影图；
		 * 句柄都落进 Blackboard 的 "ShadowMapHandle"，由光照 Pass 绑定。 */
		{
			auto shadow_map_manager = render_view.GetShadowMapManager();
			shadow_map_manager->AddNoShadowMapPass(*frame_graph);
			if (render_view.HasShadowCast())
			{
				shadow_map_manager->AddShadowPass(*frame_graph, render_view.GetOwnerScene(), &render_view);
			}
			else
			{
				const auto fallback_handle = frame_graph->GetBlackboard()
					.GetResourceHandle<FrameGraphTexture>("NoShadowMapHandle");
				frame_graph->GetBlackboard()["ShadowMapHandle"] = fallback_handle;
			}
		}

		/* GBuffer Pass */
		struct GBufferPassData
		{
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture0;
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture1;
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture2;
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture3;
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture4;
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture5;
			FrameGraphResourceHandleTyped<FrameGraphTexture> ObjectId;
			FrameGraphResourceHandleTyped<FrameGraphTexture> Depth;
		};

		auto gbuffer_pass = frame_graph->AddPass<GBufferPassData>("GBufferPass",
			[&, gbuffer_shader](FrameGraphBuilder& builder, GBufferPassData& data)
			{
				data.GBufferTexture0 = builder.CreateTexture("GBufferTexture0", color_target_desc);
				data.GBufferTexture1 = builder.CreateTexture("GBufferTexture1", color_target_desc);
				data.GBufferTexture2 = builder.CreateTexture("GBufferTexture2", color_target_desc);
				data.GBufferTexture3 = builder.CreateTexture("GBufferTexture3", color_target_desc);
				data.GBufferTexture4 = builder.CreateTexture("GBufferTexture4", color_target_desc);
				data.GBufferTexture5 = builder.CreateTexture("GBufferTexture5", world_pos_target_desc);
				data.ObjectId = builder.CreateTexture("ObjectId", object_id_desc);
				data.Depth = builder.CreateTexture("GBufferDepth", depth_target_desc);

				builder.BindOutputResource(data.GBufferTexture0, FrameGraphTexture::Usage::ColorAttachment);
				builder.BindOutputResource(data.GBufferTexture1, FrameGraphTexture::Usage::ColorAttachment);
				builder.BindOutputResource(data.GBufferTexture2, FrameGraphTexture::Usage::ColorAttachment);
				builder.BindOutputResource(data.GBufferTexture3, FrameGraphTexture::Usage::ColorAttachment);
				builder.BindOutputResource(data.GBufferTexture4, FrameGraphTexture::Usage::ColorAttachment);
				builder.BindOutputResource(data.GBufferTexture5, FrameGraphTexture::Usage::ColorAttachment);
				builder.BindOutputResource(data.ObjectId, FrameGraphTexture::Usage::ColorAttachment | FrameGraphTexture::Usage::Sampleable);
				builder.BindOutputResource(data.Depth, FrameGraphTexture::Usage::DepthAttachment);

				FrameGraphPassInfo::Descriptor pass_desc;
				pass_desc.Attachments.ColorAttachments(0) = data.GBufferTexture0;
				pass_desc.Attachments.ColorAttachments(1) = data.GBufferTexture1;
				pass_desc.Attachments.ColorAttachments(2) = data.GBufferTexture2;
				pass_desc.Attachments.ColorAttachments(3) = data.GBufferTexture3;
				pass_desc.Attachments.ColorAttachments(4) = data.GBufferTexture4;
				pass_desc.Attachments.ColorAttachments(5) = data.GBufferTexture5;
				pass_desc.Attachments.ColorAttachments(6) = data.ObjectId;
				pass_desc.Attachments.DepthAttachment() = data.Depth;
				pass_desc.ColorClearValues.resize(7);
				pass_desc.ColorClearValues[6] = glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f);
				pass_desc.ViewportRegion = m_ViewportRegion;
				builder.CreateRenderPass("GBufferPassRenderTarget", pass_desc);
			},
			[&, gbuffer_shader](const FrameGraphResources& resources, const GBufferPassData& data)
			{
				std::unordered_map<const Material*, SharedPtr<Material>> gbuffer_materials;
						/* 目标 shader 已声明、材质没写的值参数 → 用默认值补齐。块式 uniform 是 per-program 状态：
						 * 不写会残留上一个对象的取值。 */
				std::unordered_map<const Material*, std::vector<MaterialParamInfo>> gbuffer_param_fills;
				const auto render_pass_info = resources.GetPassRenderTarget();
				render_view.EmplacePassFrameBuffer("GBufferPass", render_pass_info);

				render_pass_info->Bind();
				{
					Renderer::GetRenderAPI()->Clear();

					for (const auto& mesh_object : render_view.GetVisibleMeshObjects())
					{
						/* Fill object uniform buffer */
						Renderer::FillObjectUniformBuffer(mesh_object);

						const auto& material = mesh_object.Material;
						if (material == nullptr)
							continue;

						/* 天空盒不进 G-Buffer：它输出的是最终颜色、顶点着色器把网格
						 * 折叠成全屏天空，转成 G-Buffer 材质只会得到一颗实心球。
						 * 由 SkyPass 在光照之后专绘背景。 */
						if (material->IsSkyBox())
							continue;

						auto gbuffer_material = material;
						if (material->GetShader() != gbuffer_shader)
						{
							auto [cached, inserted] = gbuffer_materials.try_emplace(material.get());
							if (inserted)
								cached->second = CreateGBufferMaterial(material, gbuffer_shader);
							gbuffer_material = cached->second;
						}

						/* 按材质对象缓存补齐结果（转换材质是源材质的复制，参数同源） */
						auto fills = gbuffer_param_fills.try_emplace(material.get());
						if (fills.second)
							fills.first->second = gbuffer_material->CollectMissingValueParamDefaults();

						DrawParams per_draw;
						per_draw.Overrides = fills.first->second;
						Renderer::Submit(gbuffer_material, mesh_object.MeshSegment->GetMeshPrimitive(),
							per_draw.Overrides.empty() ? nullptr : &per_draw);
					}
				}
				render_pass_info->Unbind();
				Renderer::GetRenderAPI()->Flush();
			}
			);

		frame_graph->GetBlackboard()["GBufferTexture0"] = gbuffer_pass->GetData().GBufferTexture0;
		frame_graph->GetBlackboard()["GBufferTexture1"] = gbuffer_pass->GetData().GBufferTexture1;
		frame_graph->GetBlackboard()["GBufferTexture2"] = gbuffer_pass->GetData().GBufferTexture2;
		frame_graph->GetBlackboard()["GBufferTexture3"] = gbuffer_pass->GetData().GBufferTexture3;
		frame_graph->GetBlackboard()["GBufferTexture4"] = gbuffer_pass->GetData().GBufferTexture4;
		frame_graph->GetBlackboard()["GBufferTexture5"] = gbuffer_pass->GetData().GBufferTexture5;
		frame_graph->GetBlackboard()["GBufferDepth"] = gbuffer_pass->GetData().Depth;

		/* Lighting Pass */
		struct LightingPassData
		{
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture0;
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture1;
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture2;
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture3;
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture4;
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture5;
			FrameGraphResourceHandleTyped<FrameGraphTexture> ShadowMapHandle;
			FrameGraphResourceHandleTyped<FrameGraphTexture> LightingResult;
		};

		auto lighting_pass = frame_graph->AddPass<LightingPassData>("LightingPass",
			[&](FrameGraphBuilder& builder, LightingPassData& data)
			{
				data.GBufferTexture0 = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("GBufferTexture0");
				data.GBufferTexture1 = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("GBufferTexture1");
				data.GBufferTexture2 = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("GBufferTexture2");
				data.GBufferTexture3 = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("GBufferTexture3");
				data.GBufferTexture4 = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("GBufferTexture4");
				data.GBufferTexture5 = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("GBufferTexture5");
				data.ShadowMapHandle = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("ShadowMapHandle");

				data.LightingResult = builder.CreateTexture("LightingResult", color_target_desc);

				builder.BindInputResource(data.GBufferTexture0, FrameGraphTexture::Usage::Sampleable);
				builder.BindInputResource(data.GBufferTexture1, FrameGraphTexture::Usage::Sampleable);
				builder.BindInputResource(data.GBufferTexture2, FrameGraphTexture::Usage::Sampleable);
				builder.BindInputResource(data.GBufferTexture3, FrameGraphTexture::Usage::Sampleable);
				builder.BindInputResource(data.GBufferTexture4, FrameGraphTexture::Usage::Sampleable);
				builder.BindInputResource(data.GBufferTexture5, FrameGraphTexture::Usage::Sampleable);
				builder.BindInputResource(data.ShadowMapHandle, FrameGraphTexture::Usage::Sampleable);
				builder.BindOutputResource(data.LightingResult, FrameGraphTexture::Usage::ColorAttachment | FrameGraphTexture::Usage::Sampleable);

				FrameGraphPassInfo::Descriptor pass_desc;
				pass_desc.Attachments.ColorAttachments(0) = data.LightingResult;
				pass_desc.ViewportRegion = m_ViewportRegion;
				builder.CreateRenderPass("LightingPassRenderTarget", pass_desc);

				builder.AsSideEffect();
			},
			[&](const FrameGraphResources& resources, const LightingPassData& data)
			{
				const auto render_pass_info = resources.GetPassRenderTarget();
				render_view.EmplacePassFrameBuffer("LightingPass", render_pass_info);

				render_pass_info->Bind();
				{
					Renderer::GetRenderAPI()->Clear();

					/* 阴影纹理随 Submit 直绑（按 Shader 反射出的 "u_ShadowMap" 绑定点） */
					ScopedShadowMapBinding shadow_map_binding(resources.Get(data.ShadowMapHandle).Texture);

					/* 反射探针 IBL：收集本帧参与"逐像素最近探针"选择的探针子集
					 * （≤ kMaxDeferredIBLProbes 个，按到相机的距离排序）。
					 * BRDF LUT 未就绪（没有探针烘焙过）时不做 IBL。 */
					std::vector<SharedPtr<ReflectionProbe>> ibl_probes;
					SharedPtr<DeviceTexture> ibl_brdf_lut;
					if (const auto scene = render_view.GetOwnerScene())
					{
						if (const auto probe_manager = scene->GetReflectionProbeManager())
						{
							ibl_brdf_lut = probe_manager->GetBRDFLutMap();
							if (ibl_brdf_lut != nullptr)
								ibl_probes = probe_manager->CollectClosestBakedProbes(GetPosition(), kMaxDeferredIBLProbes);
						}
					}

					const auto& lights = render_view.GetValidLights();
					for (size_t light_index = 0; light_index < lights.size(); ++light_index)
					{
						const auto& light = lights[light_index];

						/* 完整光照数据：光源参数 + 该光源的阴影（级联 / 点光 / 聚光，来自本视图阴影管理器） */
						Renderer::FillLightUniformBuffer(light, render_view.GetShadowMapManager().get());

						SharedPtr<Material> material = CreateSharedPtr<Material>();
						auto& raster_state = material->GetRasterState();
						raster_state.EnableDepthWrite = false;
						raster_state.EnableBlend = true;
						raster_state.BlendEquationRGB = BlendEquation::Add;
						raster_state.BlendEquationA = BlendEquation::Add;
						raster_state.BlendFuncSrcRGB = BlendFunc::One;
						raster_state.BlendFuncSrcA = BlendFunc::One;
						raster_state.BlendFuncDstRGB = BlendFunc::One;
						raster_state.BlendFuncDstA = BlendFunc::One;
						const auto shader = ShaderAssetManager::Instance().GetOrLoad(ABSOLUTE_PATH("Shaders/lighting.glsl"));
						material->SetShader(shader);
						material->SetTexture("u_GBufferTexture0", resources.Get(data.GBufferTexture0).Texture);
						material->SetTexture("u_GBufferTexture1", resources.Get(data.GBufferTexture1).Texture);
						material->SetTexture("u_GBufferTexture2", resources.Get(data.GBufferTexture2).Texture);
						material->SetTexture("u_GBufferTexture3", resources.Get(data.GBufferTexture3).Texture);
						material->SetTexture("u_GBufferTexture4", resources.Get(data.GBufferTexture4).Texture);
						material->SetTexture("u_GBufferTexture5", resources.Get(data.GBufferTexture5).Texture);
						/* 环境光 / 自发光与光源无关，只由第一笔光照合成（多光源逐笔加法叠加） */
						material->SetParameters(ParamType::Int, "u_ComposeAmbientEmission",
							light_index == 0 ? 1 : 0);

								/* 探针 IBL 的参数和贴图每笔光照都完整设一遍（裸 uniform 是 per-program 状态，漏设就残留
								 * 上一笔）；采样槽位不管有没有效都必须有绑定（缺绑定会被 Metal 校验断言），无效槽位用中性兜底。 */
						material->SetParameters(ParamType::Int, "u_ProbeCount",
							static_cast<int>(ibl_probes.size()));
						for (size_t probe_index = 0; probe_index < kMaxDeferredIBLProbes; ++probe_index)
						{
							const std::string suffix = std::to_string(probe_index);
							const bool valid = probe_index < ibl_probes.size();
							material->SetParameters(ParamType::Vec3, "u_ProbePosition" + suffix,
								valid ? ibl_probes[probe_index]->GetPosition() : glm::vec3(0.0f));

							material->SetTexture("u_IrradianceMap" + suffix,
								valid ? ibl_probes[probe_index]->GetIrradianceMap() : DeviceTexture::BlackCube());
							material->SetTexture("u_PrefilterMap" + suffix,
								valid ? ibl_probes[probe_index]->GetPrefilterMap() : DeviceTexture::BlackCube());
						}
						material->SetTexture("u_BRDFLut",
							ibl_brdf_lut != nullptr ? ibl_brdf_lut : DeviceTexture::Black());

						Renderer::Submit(material, Renderer::GetFullScreenVertexArray());
					}
				}
				render_pass_info->Unbind();
			}
			);
		frame_graph->GetBlackboard()["LightingPassOutput"] = lighting_pass->GetData().LightingResult;

			/* 天空背景 Pass：天空盒材质按原样画进光照结果。顶点着色器折叠成全屏天空、深度恒为远平面
			 * （Reversed-Z），跟 GBuffer 深度做 GreaterEqual 比较（只在背景像素落笔）。要在光照之后、
			 * 叠加层之前。 */
		struct SkyPassData
		{
			FrameGraphResourceHandleTyped<FrameGraphTexture> Output;
			FrameGraphResourceHandleTyped<FrameGraphTexture> Depth;
		};

		frame_graph->AddPass<SkyPassData>("SkyPass",
			[&](FrameGraphBuilder& builder, SkyPassData& data)
			{
				data.Output = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("LightingPassOutput");
				data.Depth = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("GBufferDepth");
				builder.BindInputResource(data.Output, FrameGraphTexture::Usage::ColorAttachment);
				builder.BindInputResource(data.Depth, FrameGraphTexture::Usage::DepthAttachment);

				FrameGraphPassInfo::Descriptor pass_desc;
				pass_desc.Attachments.ColorAttachments(0) = data.Output;
				pass_desc.Attachments.DepthAttachment() = data.Depth;
				/* 保留光照结果与场景深度（天空只在背景像素落笔） */
				pass_desc.PreserveContent = true;
				pass_desc.ViewportRegion = m_ViewportRegion;
				builder.CreateRenderPass("SkyPassRenderTarget", pass_desc);

				/* 与 AxisPass 同理：叠加型 Pass 的产物是"写进已有资源"，
				 * 不声明为目标会被图裁剪剔除 */
				builder.AsSideEffect();
			},
			[&](const FrameGraphResources& resources, const SkyPassData&)
			{
				const auto render_pass_info = resources.GetPassRenderTarget();
				render_view.EmplacePassFrameBuffer("SkyPass", render_pass_info);

				render_pass_info->Bind();
				{
					/* 天空盒材质按原样提交（不做 G-Buffer 转换）：u_SkyTex 采样、
					 * 远平面 z、面剔除姿态都由材质自身的光栅状态决定 */
					for (const auto& mesh_object : render_view.GetVisibleMeshObjects())
					{
						const auto& material = mesh_object.Material;
						if (material == nullptr || !material->IsSkyBox())
							continue;

						Renderer::FillObjectUniformBuffer(mesh_object);
						Renderer::Submit(material, mesh_object.MeshSegment->GetMeshPrimitive());
					}
				}
				render_pass_info->Unbind();
			}
			);

		/* 场景 gizmo Pass（编辑器视口叠加层：地面网格 + 坐标轴 + 实体图标，见 SceneGizmos）。
		 * 直接复用管线输出和场景深度：PreserveContent 不清除附件，gizmo 画进输出、与场景深度
		 * 做 GreaterEqual 比较（会被物体挡住）；它是最后一个写输出纹理的 Pass。 */
		struct AxisPassData
		{
			FrameGraphResourceHandleTyped<FrameGraphTexture> Output;
			FrameGraphResourceHandleTyped<FrameGraphTexture> Depth;
		};

		frame_graph->AddPass<AxisPassData>("AxisPass",
			[&](FrameGraphBuilder& builder, AxisPassData& data)
			{
				data.Output = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("LightingPassOutput");
				data.Depth = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("GBufferDepth");
				builder.BindInputResource(data.Output, FrameGraphTexture::Usage::ColorAttachment);
				builder.BindInputResource(data.Depth, FrameGraphTexture::Usage::DepthAttachment);

				FrameGraphPassInfo::Descriptor pass_desc;
				pass_desc.Attachments.ColorAttachments(0) = data.Output;
				pass_desc.Attachments.DepthAttachment() = data.Depth;
				/* 叠加层：保留颜色（光照结果）与深度（场景深度） */
				pass_desc.PreserveContent = true;
				pass_desc.ViewportRegion = m_ViewportRegion;
				builder.CreateRenderPass("AxisPassRenderTarget", pass_desc);

				/* 输出纹理由本 Pass 画完并留给 ImGui 采样：
				 * 既不能因为"没有被下游消费"被剔除，也不能在末尾随资源生命周期销毁。 */
				builder.AsSideEffect();
			},
			[&](const FrameGraphResources& resources, const AxisPassData&)
			{
				const auto render_pass_info = resources.GetPassRenderTarget();
				render_view.EmplacePassFrameBuffer("AxisPass", render_pass_info);

				render_pass_info->Bind();
				{
					/* 场景 gizmo 叠加层（网格 / 世界轴 / 实体图标）：见 SceneGizmos */
					SceneGizmos::Submit(render_view, *this);
				}
				render_pass_info->Unbind();
			}
			);

		// frame_graph->ExportGraphviz("framegraph.txt");
		render_view.Prepare();

		/* 输出 */
		render_view.SetRenderTargetHandle(lighting_pass->GetData().LightingResult);

		return true;
	}

	int32_t EditorCamera::PickingEntityByPixelPos(uint32_t x, uint32_t y) const
	{
		int32_t entity_id = -1;
		if (const auto& gbuffer_framebuffer = m_pRenderView->GetPassFrameBuffer("GBufferPass"))
		{
			gbuffer_framebuffer->ReadPixel(6, x, y, { PixelFormat::R_Integer, PixelType::Int }, &entity_id);
		}
		return entity_id;
	}



	glm::quat EditorCamera::GetOrientation() const
	{
		return glm::quat(glm::vec3(-m_Pitch, -m_Yaw, 0.0f));
	}

	void EditorCamera::SetViewMatrix(const glm::mat4& view_mat)
	{
		PROFILE_FUNCTION();

		if (view_mat != m_ViewMatrix)
		{
			m_ViewMatrix = view_mat;

			/* 根据ViewMatrix恢复Pitch和Yaw */
			/* todo： 数据未恢复全，仍存在问题，需要重新维护这部分 */
			const auto orientation = glm::toQuat(view_mat);
			const glm::vec3 euler_angles = glm::eulerAngles(orientation);
			m_Pitch = -euler_angles.x;
			m_Yaw = -euler_angles.y;
			m_UpDirection = glm::rotate(orientation, glm::vec3(0.0f, 1.0f, 0.0f));
			m_RightDirection = glm::rotate(orientation, glm::vec3(1.0f, 0.0f, 0.0f));
			m_ForwardDirection = glm::rotate(orientation, glm::vec3(0.0f, 0.0f, -1.0f));
		}
	}

	/* 视图指示器：直接设置轨道朝向（俯仰按导航限位夹取），聚焦点与距离不变 ——
	 * 位置是派生量，下一帧自动落到"枢轴 - 前向 × 距离"的新视点 */
	void EditorCamera::SetOrbitAngles(float yaw, float pitch)
	{
		PROFILE_FUNCTION();

		m_Yaw = yaw;
		m_Pitch = glm::clamp(pitch, -kMaxPitch, kMaxPitch);
		UpdateCameraDirections();

		m_IsDirty = true;
	}

	/* 视图指示器拖拽：像素增量走与鼠标轨道相同的灵敏度与速度系数（手感一致） */
	void EditorCamera::OrbitByPixelDelta(const glm::vec2& pixel_delta)
	{
		PROFILE_FUNCTION();

		OnMouseRotate(pixel_delta * kMouseSensitivity);
	}

	void EditorCamera::UpdateProjectionMatrix()
	{
		PROFILE_FUNCTION();

		m_ProjectionMatrix = MakeReversedZProjection(glm::perspective(glm::radians(m_Fov), m_AspectRatio, m_NearClip, m_FarClip));
	}

	void EditorCamera::UpdateViewMatrix()
	{
		PROFILE_FUNCTION();

		if (m_IsDirty)
		{
			/* 位置恒为派生量：聚焦点 - 前向 * 距离。飞行中的平移与转向也维护这组关系
			 * （见 UpdateFly），因此两种导航状态共享同一套参数，进出飞行无需交接。 */
			m_Position = m_FocalPoint - m_ForwardDirection * m_Distance;
			m_ViewMatrix = glm::inverse(glm::translate(glm::mat4(1.0f), m_Position) * glm::toMat4(GetOrientation()));

			m_IsDirty = false;
		}
	}

	void EditorCamera::UpdateCameraDirections()
	{
		PROFILE_FUNCTION();

		const auto orientation = GetOrientation();
		m_UpDirection = glm::rotate(orientation, glm::vec3(0.0f, 1.0f, 0.0f));
		m_RightDirection = glm::rotate(orientation, glm::vec3(1.0f, 0.0f, 0.0f));
		m_ForwardDirection = glm::rotate(orientation, glm::vec3(0.0f, 0.0f, -1.0f));
	}

	void EditorCamera::OnMousePan(const glm::vec2& delta)
	{
		PROFILE_FUNCTION();

		const auto speed = PanSpeed();
		m_FocalPoint += -GetRightDir() * delta.x * speed.x * m_Distance;
		m_FocalPoint += GetUpDir() * delta.y * speed.y * m_Distance;

		m_IsDirty = true;
	}

	void EditorCamera::OnMouseRotate(const glm::vec2& delta)
	{
		PROFILE_FUNCTION();

		const float yaw_sign = GetUpDir().y < 0 ? -1.0f : 1.0f;
		m_Yaw += yaw_sign * delta.x * RotateSpeed();
		m_Pitch = glm::clamp(m_Pitch + delta.y * RotateSpeed(), -kMaxPitch, kMaxPitch);

		UpdateCameraDirections();

		m_IsDirty = true;
	}

	void EditorCamera::OnMouseZoom(float delta)
	{
		PROFILE_FUNCTION();

		m_Distance = glm::clamp(m_Distance - delta * ZoomSpeed(), kMinDistance, kMaxDistance);

		m_IsDirty = true;
	}

	/* 滚轮缩放：乘性步进（滚一格约 ±10%），触控板的小增量也能平滑生效 */
	void EditorCamera::OnMouseWheelZoom(float wheel)
	{
		PROFILE_FUNCTION();

		m_Distance = glm::clamp(m_Distance * std::pow(kWheelZoomStep, wheel), kMinDistance, kMaxDistance);

		m_IsDirty = true;
	}

	/* 滚轮调飞行速度（飞行中）：乘性步进 + 上下限 */
	void EditorCamera::AdjustMoveSpeed(float wheel)
	{
		PROFILE_FUNCTION();

		m_MoveSpeed = glm::clamp(m_MoveSpeed * std::pow(kWheelSpeedStep, wheel), kMinFlySpeed, kMaxFlySpeed);
	}

	/* 聚焦：轨道中心移到目标点，距离按包围球半径取景（水平 / 垂直视场角取紧的一侧，含边距） */
	void EditorCamera::FocusOn(const glm::vec3& target, float radius)
	{
		PROFILE_FUNCTION();

		m_FocalPoint = target;

		const float half_fov_v = glm::radians(m_Fov) * 0.5f;
		const float half_fov_h = std::atan(std::tan(half_fov_v) * glm::max(m_AspectRatio, 0.01f));
		const float half_fov = glm::max(glm::min(half_fov_v, half_fov_h), 0.01f);
		m_Distance = glm::clamp(radius / std::tan(half_fov) * 1.2f, kMinDistance, kMaxDistance);

		m_IsDirty = true;
	}

	glm::vec2 EditorCamera::PanSpeed() const
	{
		PROFILE_FUNCTION();

		const float x = std::min(m_ViewportRegion.Width / 1000.0f, 2.4f); // max = 2.4f
		const float speed_x = 0.0366f * (x * x) - 0.1778f * x + 0.3021f;

		const float y = std::min(m_ViewportRegion.Height / 1000.0f, 2.4f); // max = 2.4f
		const float speed_y = 0.0366f * (y * y) - 0.1778f * y + 0.3021f;

		return glm::vec2(speed_x, speed_y);
	}

	float EditorCamera::RotateSpeed() const
	{
		return 0.8f;
	}

	float EditorCamera::ZoomSpeed() const
	{
		PROFILE_FUNCTION();

		float distance = m_Distance * 0.2f;
		distance = std::max(distance, 0.0f);
		float speed = distance * distance;
		speed = std::min(speed, 100.0f); // max speed = 100
		return speed;
	}
}
