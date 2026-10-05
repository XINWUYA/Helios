#pragma once

namespace Helios
{
	/* 编辑器内建相机类 */
	class EditorCamera final : public Camera
	{
	public:
		EditorCamera(float fov = 45.0f, float aspect_ratio = 1.778f, float near_clip = 0.1f, float far_clip = 1000.0f);
		~EditorCamera() override;

		void OnUpdate(float delta_time);
		void OnEvent(class IEvent* event) {}

		/* 设置视口区域 */
		void SetViewportRegion(const ViewportRegion& region);
		[[nodiscard]]
		const ViewportRegion& GetViewportRegion() const { return m_ViewportRegion; }

		/* 设置聚焦模式 */
		[[nodiscard]]
		bool IsFocus() const { return m_IsFocus; }
		void SetFocus(bool focus) { m_IsFocus = focus; }

		/* 相机距离 */
		void SetDistance(float distance) { m_Distance = distance; m_IsDirty = true; }
		[[nodiscard]]
		float GetDistance() const { return m_Distance; }
		/* 移动速度 */
		void SetMoveSpeed(float speed) { m_MoveSpeed = speed; }
		[[nodiscard]]
		float GetMoveSpeed() const { return m_MoveSpeed; }

		void SetFocalPoint(const glm::vec3& position) { m_FocalPoint = position; m_IsDirty = true; }

		[[nodiscard]]
		float GetPitch() const { return m_Pitch; }
		[[nodiscard]]
		float GetYaw() const { return m_Yaw; }

		[[nodiscard]]
		glm::quat GetOrientation() const;

		void SetViewMatrix(const glm::mat4& view_mat);

		/* 根据像素位置，获取EntityId */
		[[nodiscard]]
		int32_t PickingEntityByPixelPos(uint32_t x, uint32_t y) const;

	private:
		/* 构建内置的FrameGraph */
		bool ConstructRenderView(RenderView& render_view) override;

		void UpdateProjectionMatrix();
		void UpdateViewMatrix();
		void UpdateCameraDirections();

		// 聚焦模式
		void OnMousePan(const glm::vec2& delta); // 整体平移
		void OnMouseRotate(const glm::vec2& delta); // 绕聚焦中心旋转
		void OnMouseZoom(float delta); // 拉远拉近

		[[nodiscard]]
		glm::vec2 PanSpeed() const;
		[[nodiscard]]
		float RotateSpeed() const;
		[[nodiscard]]
		float ZoomSpeed() const;

		float m_Pitch{ 0.0f }, m_Yaw{ 0.0f };
		/* 相机位置 */
		float m_Distance{ 10.0f };
		/* 相机移动速度 */
		float m_MoveSpeed{ 1.0f };

		/* 是否启用聚焦模式 */
		bool m_IsFocus{ true };
		/* 是否需要更新变换矩阵 */
		bool m_IsDirty{ true };

		glm::vec3 m_FocalPoint{ 0.0f, 0.0f, 0.0f };
		glm::vec2 m_LastMousePosition{ 0.0f, 0.0f };

		/* 视口区域 */
		ViewportRegion m_ViewportRegion{};
	};
}
