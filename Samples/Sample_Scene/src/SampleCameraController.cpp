#include "Pch.h"
#include "SampleCameraController.h"

namespace Helios
{
	SampleCameraController::SampleCameraController(const Entity& camera_entity, bool is_focus)
		: m_CameraEntity(camera_entity), m_IsFocus(is_focus)
	{
		PROFILE_FUNCTION();

		const auto& camera_component = camera_entity.GetComponent<CameraComponent>();
		m_pCamera = camera_component.m_Camera;
	}

	void SampleCameraController::OnUpdate(float delta_time)
	{
		PROFILE_FUNCTION();

		auto position = m_pCamera->GetPosition();
		auto forward = m_pCamera->GetForwardDir();
		auto right = m_pCamera->GetRightDir();
		auto up = m_pCamera->GetUpDir();

		if (m_IsFocus)
		{
			const bool alt_pressed = Input::IsKeyPressed(Key::LeftAlt) || Input::IsKeyPressed(Key::RightAlt);
			if (alt_pressed)
			{
				const auto mouse_pos = Input::GetMousePos();
				if (!m_IsAltHeldLastFrame)
					m_LastMousePosition = mouse_pos;
				m_IsAltHeldLastFrame = true;

				const auto delta = (mouse_pos - m_LastMousePosition) * 0.03f;
				m_LastMousePosition = mouse_pos;

				if (Input::IsMouseButtonPressed(Mouse::ButtonMiddle))
					OnMousePan(delta);
				else if (Input::IsMouseButtonPressed(Mouse::ButtonLeft))
					OnMouseRotate(delta);
				else if (Input::IsMouseButtonPressed(Mouse::ButtonRight))
					OnMouseZoom(delta.y);
			}
			else
			{
				m_IsAltHeldLastFrame = false;
			}

			const float yaw_rad = glm::radians(m_Yaw);
			const float pitch_rad = glm::radians(m_Pitch);

			forward = glm::normalize(glm::vec3(
				std::cos(pitch_rad) * std::sin(yaw_rad),
				std::sin(pitch_rad),
				-std::cos(pitch_rad) * std::cos(yaw_rad)
			));
			position = m_FocalPoint - forward * m_FocalDistance;

			const glm::vec3 world_up(0.0f, 1.0f, 0.0f);
			right = glm::normalize(glm::cross(forward, world_up));
			up = glm::normalize(glm::cross(right, forward));
		}
		else
		{
			if (Input::IsMouseButtonPressed(Mouse::ButtonRight))
			{
				/* 向前/向后 */
				if (Input::IsKeyPressed(Key::W))
				{
					position += forward * m_MoveSpeed * delta_time;
				}
				else if (Input::IsKeyPressed(Key::S))
				{
					position -= forward * m_MoveSpeed * delta_time;
				}

				/* 向左/向右 */
				if (Input::IsKeyPressed(Key::A))
				{
					position -= right * m_MoveSpeed * delta_time;
				}
				else if (Input::IsKeyPressed(Key::D))
				{
					position += right * m_MoveSpeed * delta_time;
				}

				/* 向上/向下 */
				if (Input::IsKeyPressed(Key::Q))
				{
					position += up * m_MoveSpeed * delta_time;
				}
				else if (Input::IsKeyPressed(Key::E))
				{
					position -= up * m_MoveSpeed * delta_time;
				}
			}
		}

		glm::mat4 camera_transform = glm::mat4(1.0f);
		camera_transform[0] = glm::vec4(right, 0.0f);
		camera_transform[1] = glm::vec4(up, 0.0f);
		camera_transform[2] = glm::vec4(-forward, 0.0f);
		camera_transform[3] = glm::vec4(position, 1.0f);

		auto& transform_component = m_CameraEntity.GetComponent<TransformComponent>();
		transform_component.m_Position = position;
		transform_component.m_Rotation = glm::eulerAngles(glm::quat(glm::mat3(camera_transform)));
	}

	void SampleCameraController::OnMousePan(const glm::vec2& delta)
	{
		PROFILE_FUNCTION();
		if (!m_pCamera) return;

		const auto speed = PanSpeed();
		m_FocalPoint += -m_pCamera->GetRightDir() * delta.x * speed.x * m_FocalDistance;
		m_FocalPoint += m_pCamera->GetUpDir() * delta.y * speed.y * m_FocalDistance;
	}

	void SampleCameraController::OnMouseRotate(const glm::vec2& delta)
	{
		PROFILE_FUNCTION();
		if (!m_pCamera) return;

		const float yaw_sign = m_pCamera->GetUpDir().y < 0 ? -1.0f : 1.0f;
		m_Yaw += yaw_sign * delta.x * RotateSpeed();
		m_Pitch += delta.y * RotateSpeed();
		m_Pitch = glm::clamp(m_Pitch, -89.0f, 89.0f);
	}

	void SampleCameraController::OnMouseZoom(float delta)
	{
		PROFILE_FUNCTION();
		if (!m_pCamera) return;

		m_FocalDistance -= delta * ZoomSpeed();
		if (m_FocalDistance < 1.0f)
		{
			m_FocalPoint += m_pCamera->GetForwardDir();
			m_FocalDistance = 1.0f;
		}
	}

	glm::vec2 SampleCameraController::PanSpeed() const
	{
		PROFILE_FUNCTION();

		const float x = std::min(m_ViewportRegion.Width / 1000.0f, 2.4f); // max = 2.4f
		const float speed_x = 0.0366f * (x * x) - 0.1778f * x + 0.3021f;

		const float y = std::min(m_ViewportRegion.Height / 1000.0f, 2.4f); // max = 2.4f
		const float speed_y = 0.0366f * (y * y) - 0.1778f * y + 0.3021f;

		return glm::vec2(speed_x, speed_y);
	}

	float SampleCameraController::RotateSpeed() const
	{
		return 0.8f;
	}

	float SampleCameraController::ZoomSpeed() const
	{
		PROFILE_FUNCTION();

		float distance = m_FocalDistance * 0.2f;
		distance = std::max(distance, 0.0f);
		float speed = distance * distance;
		speed = std::min(speed, 100.0f); // max speed = 100
		return speed;
	}
}
