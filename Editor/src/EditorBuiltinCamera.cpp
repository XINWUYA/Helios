#include "Pch.h"
#include "EditorBuiltinCamera.h"
#include "SceneEditor/SceneGizmos.h"
#include <glm/gtx/quaternion.hpp>
#include <cmath>

#include "Helios/Common/Math.h"
#include "Helios/Renderer/RenderPasses/DeferredPasses.h"
#include "Helios/Renderer/RenderPasses/ScenePass.h"

namespace Helios
{
	namespace
	{
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

		/* 编辑器视口缺省走延迟管线（可在 View 菜单切前向 / 延迟） */
		SetRenderPipeline(RenderPipeline::Deferred);

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

	/* 构建内置的 FrameGraph（前向 / 延迟两条管线可选）。管线本体由 Kernel 组织（各自模块）；
	 * 本相机只管视口判定、管线分派和编辑器叠加层。由 RenderView 执行时回调，渲染图每帧按
	 * 当前视口尺寸重建。 */
	bool EditorCamera::ConstructRenderView(RenderView& render_view)
	{
		PROFILE_FUNCTION();

		/* 视口尺寸无效时不组织渲染图：这里必须返回 true（表示"本相机负责组织"），
		 * 否则会回落到默认前向图并沿用 0 尺寸的渲染目标。渲染图为空时输出为空。 */
		if (m_ViewportRegion.Width <= 0 || m_ViewportRegion.Height <= 0)
			return true;

		/* 渲染图已由 RenderView 先重置再回调本函数，这里直接追加各 Pass */
		auto& frame_graph = render_view.GetFrameGraph();

		/* 按管线分派：两条管线的输出（颜色 + 深度）都落 Blackboard，供叠加层与拾取复用 */
		FrameGraphResourceHandleTyped<FrameGraphTexture> overlay_output;
		FrameGraphResourceHandleTyped<FrameGraphTexture> overlay_depth;
		if (render_view.GetRenderPipeline() == RenderPipeline::Deferred)
		{
			Deferred::AddDeferredPasses(&render_view);
			overlay_output = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("LightingPassOutput");
			overlay_depth = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("GBufferDepth");
		}
		else
		{
			render_view.AddShadowMapPasses();
			Forward::AddScenePassToTexture(&render_view);
			overlay_output = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("ScenePassOutput");
			overlay_depth = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("ScenePassDepth");
		}

		/* 场景 gizmo Pass（编辑器视口叠加层：地面网格 + 坐标轴 + 实体图标，见 SceneGizmos）。
		 * 直接复用管线输出和场景深度：PreserveContent 不清除附件，gizmo 画进输出、与场景深度
		 * 做 GreaterEqual 比较（会被物体挡住）；它是最后一个写输出纹理的 Pass。 */
		struct AxisPassData
		{
			FrameGraphResourceHandleTyped<FrameGraphTexture> Output;
			FrameGraphResourceHandleTyped<FrameGraphTexture> Depth;
		};

		frame_graph->AddPass<AxisPassData>("AxisPass",
			[&, overlay_output, overlay_depth](FrameGraphBuilder& builder, AxisPassData& data)
			{
				data.Output = overlay_output;
				data.Depth = overlay_depth;
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

		return true;
	}

	int32_t EditorCamera::PickingEntityByPixelPos(uint32_t x, uint32_t y) const
	{
		int32_t entity_id = -1;

		/* 拾取通道随管线不同：延迟 = GBufferPass 的第 6 张附件（ObjectId）；
		 * 前向 = ScenePass 的第 1 张附件（颜色在 0）。两者清值都是 -1。 */
		const bool deferred = m_pRenderView->GetRenderPipeline() == RenderPipeline::Deferred;
		const SharedPtr<DeviceFrameBuffer> pass_framebuffer = m_pRenderView->GetPassFrameBuffer(
			deferred ? "GBufferPass" : "ScenePass");
		if (pass_framebuffer)
		{
			pass_framebuffer->ReadPixel(deferred ? 6 : 1, x, y, { PixelFormat::R_Integer, PixelType::Int }, &entity_id);
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
