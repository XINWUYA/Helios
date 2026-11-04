#pragma once
#include <Helios.h>

namespace Helios
{
	/* 相机控制类 */
	class SampleCameraController final
	{
	public:
		SampleCameraController(const Entity& camera_entity, bool is_focus = false);

		void OnUpdate(float delta_time);
		void OnEvent(class IEvent* event) {}

		/* 设置视口区域 */
		void SetViewportRegion(const ViewportRegion& region) { m_ViewportRegion = region; }
		const ViewportRegion& GetViewportRegion() const { return m_ViewportRegion; }

		/* 设置聚焦模式 */
		bool IsFocus() const { return m_IsFocus; }
		void SetFocus(bool focus) { m_IsFocus = focus; }

		/* 相机距离 */
		void SetDistance(float distance) { m_FocalDistance = distance; }
		float GetDistance() const { return m_FocalDistance; }
		/* 移动速度 */
		void SetMoveSpeed(float speed) { m_MoveSpeed = speed; }
		float GetMoveSpeed() const { return m_MoveSpeed; }

		void SetFocalPoint(const glm::vec3& position) { m_FocalPoint = position; }

		/* 朝向（角度），pitch 为负是俯视 */
		void SetAngles(float yaw, float pitch) { m_Yaw = yaw; m_Pitch = pitch; }
		float GetPitch() const { return m_Pitch; }
		float GetYaw() const { return m_Yaw; }

	private:
		// 聚焦模式
		void OnMousePan(const glm::vec2& delta); // 整体平移
		void OnMouseRotate(const glm::vec2& delta); // 绕聚焦中心旋转
		void OnMouseZoom(float delta); // 拉远拉近

		glm::vec2 PanSpeed() const;
		float RotateSpeed() const;
		float ZoomSpeed() const;

		Entity m_CameraEntity;
		SharedPtr<Camera> m_pCamera{ nullptr };

		float m_Pitch{ 0.0f }, m_Yaw{ 0.0f };
		/* 相机移动速度 */
		float m_MoveSpeed{ 1.0f };

		/* 是否启用聚焦模式 */
		bool m_IsFocus{ true };
		float m_FocalDistance{ 10.0f };
		glm::vec3 m_FocalPoint{ 0.0f, 0.0f, 0.0f };

		glm::vec2 m_LastMousePosition{ 0.0f, 0.0f };
		bool m_IsAltHeldLastFrame{ false };

		/* 视口区域 */
		ViewportRegion m_ViewportRegion{};
	};
}
