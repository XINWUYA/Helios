#include "Pch.h"
#include "OrthographicCameraController.h"
#include "Camera.h"
#include "Helios/Core/Input.h"
#include "Helios/Events/MouseEvent.h"
#include "Helios/Events/ApplicationEvent.h"

namespace Helios
{
	OrthographicCameraController::OrthographicCameraController(float aspect_ratio, bool rotatable)
		: m_AspectRatio(aspect_ratio), m_Rotatable(rotatable)
	{
		m_pCamera = CreateSharedPtr<Camera>(CameraProjectionType::Orthographic, m_AspectRatio, -1.0f, 1.0f);
		m_pCamera->SetHeightSize(2.0f * m_ZoomLevel);
	}

	void OrthographicCameraController::OnUpdate(float delta_time)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

		if (Input::IsKeyPressed(Key::W)) // 上移
		{
			m_CameraPosition.x += sin(glm::radians(m_CameraRotation)) * m_CameraTranslateSpeed * delta_time;
			m_CameraPosition.y -= cos(glm::radians(m_CameraRotation)) * m_CameraTranslateSpeed * delta_time;
		}
		else if (Input::IsKeyPressed(Key::S)) // 下移
		{
			m_CameraPosition.x -= sin(glm::radians(m_CameraRotation)) * m_CameraTranslateSpeed * delta_time;
			m_CameraPosition.y += cos(glm::radians(m_CameraRotation)) * m_CameraTranslateSpeed * delta_time;
		}
		else if (Input::IsKeyPressed(Key::A)) // 左移
		{
			m_CameraPosition.x -= cos(glm::radians(m_CameraRotation)) * m_CameraTranslateSpeed * delta_time;
			m_CameraPosition.y -= sin(glm::radians(m_CameraRotation)) * m_CameraTranslateSpeed * delta_time;
		}
		else if (Input::IsKeyPressed(Key::D)) // 右移
		{
			m_CameraPosition.x += cos(glm::radians(m_CameraRotation)) * m_CameraTranslateSpeed * delta_time;
			m_CameraPosition.y += sin(glm::radians(m_CameraRotation)) * m_CameraTranslateSpeed * delta_time;
		}

		if (m_Rotatable)
		{
			if (Input::IsKeyPressed(Key::Q)) // 逆时针
			{
				m_CameraRotation += m_CameraRotateSpeed * delta_time;
			}
			else if (Input::IsKeyPressed(Key::E)) // 顺时针
			{
				m_CameraRotation -= m_CameraRotateSpeed * delta_time;
			}

			// 将m_CameraRotation限制在-180.0f~180.0f，防止数据溢出
			if (m_CameraRotation > 180.0f)
				m_CameraRotation -= 360.0f;
			else if (m_CameraRotation <= -180.0f)
				m_CameraRotation += 360.0f;

			m_pCamera->SetRotation(m_CameraRotation);
		}

		m_pCamera->SetPosition(m_CameraPosition);
	}

	void OrthographicCameraController::OnEvent(IEvent* event)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<MouseScrolledEvent>(BIND_EVENT_FUNC(OrthographicCameraController::OnMouseScrolled));
		dispatcher.Dispatch<WindowResizeEvent>(BIND_EVENT_FUNC(OrthographicCameraController::OnWindowResized));
	}

	void OrthographicCameraController::OnResize(float width, float height)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

		m_AspectRatio = width / height;
		m_pCamera->SetAspectRatio(m_AspectRatio);
		m_pCamera->SetHeightSize(2.0f * m_ZoomLevel);
	}

	bool OrthographicCameraController::OnMouseScrolled(MouseScrolledEvent* event)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

		m_ZoomLevel -= event->GetYOffset() * 0.25f;
		m_ZoomLevel = std::max(m_ZoomLevel, 0.25f);
		m_pCamera->SetAspectRatio(m_AspectRatio);
		m_pCamera->SetHeightSize(2.0f * m_ZoomLevel);
		return false;
	}

	bool OrthographicCameraController::OnWindowResized(WindowResizeEvent* event)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

		OnResize(static_cast<float>(event->GetWidth()), static_cast<float>(event->GetHeight()));
		return false;
	}
}
