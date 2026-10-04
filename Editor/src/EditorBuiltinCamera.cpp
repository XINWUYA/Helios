#include "Pch.h"
#include "EditorBuiltinCamera.h"
#include <glm/gtx/quaternion.hpp>

#include "Helios/Application/Application.h"
#include "Helios/Common/Math.h"
#include "Helios/ImGui/ImGuiLayer.h"

namespace Helios
{
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

		if (m_IsFocus)
		{
			if (Input::IsKeyPressed(Key::LeftAlt) || Input::IsKeyPressed(Key::RightAlt))
			{
				const auto mouse_pos = Input::GetMousePos();
				const auto delta = (mouse_pos - m_LastMousePosition) * 0.003f;
				m_LastMousePosition = mouse_pos;

				if (Input::IsMouseButtonPressed(Mouse::ButtonMiddle))
					OnMousePan(delta);
				else if (Input::IsMouseButtonPressed(Mouse::ButtonLeft))
					OnMouseRotate(delta);
				else if (Input::IsMouseButtonPressed(Mouse::ButtonRight))
					OnMouseZoom(delta.y);
			}
		}
		else
		{
			if (Input::IsMouseButtonPressed(Mouse::ButtonRight))
			{
				/* 向前/向后 */
				if (Input::IsKeyPressed(Key::W))
				{
					m_Position -= m_ForwardDirection * m_MoveSpeed * delta_time;
					m_IsDirty = true;
				}
				else if (Input::IsKeyPressed(Key::S))
				{
					m_Position += m_ForwardDirection * m_MoveSpeed * delta_time;
					m_IsDirty = true;
				}

				/* 向左/向右 */
				if (Input::IsKeyPressed(Key::A))
				{
					m_Position -= m_RightDirection * m_MoveSpeed * delta_time;
					m_IsDirty = true;
				}
				else if (Input::IsKeyPressed(Key::D))
				{
					m_Position += m_RightDirection * m_MoveSpeed * delta_time;
					m_IsDirty = true;
				}

				/* 向上/向下 */
				if (Input::IsKeyPressed(Key::Q))
				{
					m_Position += m_UpDirection * m_MoveSpeed * delta_time;
					m_IsDirty = true;
				}
				else if (Input::IsKeyPressed(Key::E))
				{
					m_Position -= m_UpDirection * m_MoveSpeed * delta_time;
					m_IsDirty = true;
				}
			}
		}
		//EDITOR_LOG_DEBUG("Camera Position: {}, {}, {}", m_Position.x, m_Position.y, m_Position.z);
		UpdateViewMatrix();
	}

	/* 设置视口区域 */
	void EditorCamera::SetViewportRegion(const ViewportRegion& region)
	{
		PROFILE_FUNCTION();

		m_ViewportRegion = region;
		m_pRenderView->SetViewportRegion(region);

		m_AspectRatio = static_cast<float>(m_ViewportRegion.Width) / static_cast<float>(m_ViewportRegion.Height);
		UpdateProjectionMatrix();

		/* 更新视口区域时，需重新构建FrameGraph, 保证RenderTarget的size是正确的 */
		m_IsFrameGraphDirty = true;
	}

	/* 构建内置的FrameGraph */
	void EditorCamera::ConstructRenderView()
	{
		PROFILE_FUNCTION();

		if (!m_IsFrameGraphDirty)
			return;

		if (m_ViewportRegion.Width <= 0 || m_ViewportRegion.Height <= 0)
			return;

		auto& frame_graph = m_pRenderView->GetFrameGraph();
		frame_graph->Reset();

		FrameGraphTexture::Descriptor color_target_desc;
		color_target_desc.Width = m_ViewportRegion.Width;
		color_target_desc.Height = m_ViewportRegion.Height;
		color_target_desc.TextureFormat = TextureFormat::RGBA8;

		FrameGraphTexture::Descriptor depth_target_desc;
		depth_target_desc.Width = m_ViewportRegion.Width;
		depth_target_desc.Height = m_ViewportRegion.Height;
		depth_target_desc.TextureFormat = TextureFormat::Depth32;

		FrameGraphTexture::Descriptor object_id_desc;
		object_id_desc.Width = m_ViewportRegion.Width;
		object_id_desc.Height = m_ViewportRegion.Height;
		object_id_desc.TextureFormat = TextureFormat::R32I;

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
			[&](FrameGraphBuilder& builder, GBufferPassData& data)
			{
				data.GBufferTexture0 = builder.CreateTexture("GBufferTexture0", color_target_desc);
				data.GBufferTexture1 = builder.CreateTexture("GBufferTexture1", color_target_desc);
				data.GBufferTexture2 = builder.CreateTexture("GBufferTexture2", color_target_desc);
				data.GBufferTexture3 = builder.CreateTexture("GBufferTexture3", color_target_desc);
				data.GBufferTexture4 = builder.CreateTexture("GBufferTexture4", color_target_desc);
				data.GBufferTexture5 = builder.CreateTexture("GBufferTexture5", color_target_desc);
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
				pass_desc.ViewportRegion = m_ViewportRegion;
				builder.CreateRenderPass("GBufferPassRenderTarget", pass_desc);
			},
			[&](const FrameGraphResources& resources, const GBufferPassData& data)
			{
				const auto render_pass_info = resources.GetPassRenderTarget();
				m_pRenderView->EmplacePassFrameBuffer("GBufferPass", render_pass_info);

				render_pass_info->Bind();
				{
					Renderer::GetRenderAPI()->Clear();

					/* Clear ObjectId Texture */
					int32_t clear_data = -1;
					render_pass_info->ClearAttachment(6, 0, { PixelFormat::R_Integer, PixelType::Int }, &clear_data);

					for (const auto& mesh_object : m_pRenderView->GetVisibleMeshObjects())
					{
						/* Fill object uniform buffer */
						Renderer::FillObjectUniformBuffer(mesh_object);

						auto& material = mesh_object.MeshSegment->GetMaterial();
						//material->SetParameters("u_Local2WorldMat", mesh_object.Local2WorldMat);
						//material->SetParameters("u_ViewProjectionMat", GetViewProjectionMatrix());
						Renderer::Submit(material, mesh_object.MeshSegment->GetMeshPrimitive());
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

		/* Lighting Pass */
		struct LightingPassData
		{
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture0;
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture1;
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture2;
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture3;
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture4;
			FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture5;
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

				data.LightingResult = builder.CreateTexture("LightingResult", color_target_desc);

				builder.BindInputResource(data.GBufferTexture0, FrameGraphTexture::Usage::Sampleable);
				builder.BindInputResource(data.GBufferTexture1, FrameGraphTexture::Usage::Sampleable);
				builder.BindInputResource(data.GBufferTexture2, FrameGraphTexture::Usage::Sampleable);
				builder.BindInputResource(data.GBufferTexture3, FrameGraphTexture::Usage::Sampleable);
				builder.BindInputResource(data.GBufferTexture4, FrameGraphTexture::Usage::Sampleable);
				builder.BindInputResource(data.GBufferTexture5, FrameGraphTexture::Usage::Sampleable);
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
				m_pRenderView->EmplacePassFrameBuffer("LightingPass", render_pass_info);

				render_pass_info->Bind();
				{
					Renderer::GetRenderAPI()->Clear();
					for (const auto& light : m_pRenderView->GetValidLights())
					{
						/* Fill light uniform buffer */
						Renderer::FillLightUniformBuffer(light);

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
						Renderer::Submit(material, Renderer::GetFullScreenVertexArray());
					}
				}
				render_pass_info->Unbind();
			}
			);
		frame_graph->GetBlackboard()["LightingPassOutput"] = lighting_pass->GetData().LightingResult;

		// frame_graph->ExportGraphviz("framegraph.txt");
		m_pRenderView->Prepare();

		/* 输出 */
		m_pRenderView->SetRenderTargetHandle(lighting_pass->GetData().LightingResult);

		m_IsFrameGraphDirty = false;
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
			if (m_IsFocus)
			{
				m_Position = m_FocalPoint - m_ForwardDirection * m_Distance; /* 相机始终围绕聚焦点 */
				m_ViewMatrix = glm::translate(glm::mat4(1.0f), m_Position) * glm::toMat4(GetOrientation());
				m_ViewMatrix = glm::inverse(m_ViewMatrix);
			}
			else
			{
				m_ViewMatrix = glm::translate(glm::mat4(1.0f), m_Position) * glm::toMat4(GetOrientation());
				m_ViewMatrix = glm::inverse(m_ViewMatrix);
			}

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
		m_Pitch += delta.y * RotateSpeed();

		UpdateCameraDirections();

		m_IsDirty = true;
	}

	void EditorCamera::OnMouseZoom(float delta)
	{
		PROFILE_FUNCTION();

		m_Distance -= delta * ZoomSpeed();
		if (m_Distance < 1.0f)
		{
			m_FocalPoint += GetForwardDir();
			m_Distance = 1.0f;
		}

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
