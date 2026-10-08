#pragma once

namespace Helios
{
	/* 编辑器内建相机类。
	 * 远裁剪面取 1e5：场景相机的 Far 可达数千 / 数万（真世界尺度的视锥 gizmo
	 * 要能看到完整视锥），反向 Z 的浮点深度在整个区间保持精度。 */
	class EditorCamera final : public Camera
	{
	public:
		EditorCamera(float fov = 45.0f, float aspect_ratio = 1.778f, float near_clip = 0.1f, float far_clip = 100000.0f);
		~EditorCamera() override;

		/* 每帧推进相机：视口导航 + 视图矩阵。
		 * 起手新的导航手势需先经 SetNavigationAllowed 授权（指针在视口上）；
		 * 已起手的手势不受撤销影响，直到其按键松开。 */
		void OnUpdate(float delta_time);

		/* 授权 / 撤销“允许起手新的导航手势”。缺省不允许 —— 没有视口在驱动的相机不接受输入 */
		void SetNavigationAllowed(bool allowed) { m_IsNavigationAllowed = allowed; }

		/* 设置视口区域 */
		void SetViewportRegion(const ViewportRegion& region);
		[[nodiscard]]
		const ViewportRegion& GetViewportRegion() const { return m_ViewportRegion; }

		/* 相机距离（轨道半径） */
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

		/* 视图指示器：设置轨道朝向（点击轴切换视图用）—— 聚焦点与距离保持不变 */
		void SetOrbitAngles(float yaw, float pitch);
		/* 视图指示器：按屏幕像素增量轨道旋转（沿用鼠标轨道的手感系数） */
		void OrbitByPixelDelta(const glm::vec2& pixel_delta);

		/* 视口导航状态：场景层据此决定让不让出选择 / Gizmo / 滚轮 */
		[[nodiscard]]
		bool IsNavigating() const { return m_ActiveGesture != NavGesture::None; }
		[[nodiscard]]
		bool IsOrbiting() const { return m_ActiveGesture == NavGesture::Orbit; }
		[[nodiscard]]
		bool IsFlying() const { return m_ActiveGesture == NavGesture::Fly; }

		/* 滚轮：轨道模式拉近拉远（乘性），飞行模式调整移动速度 */
		void OnMouseWheelZoom(float wheel);
		void AdjustMoveSpeed(float wheel);

		/* 聚焦到目标点：轨道中心移到 target，并按包围球半径取景（F 键） */
		void FocusOn(const glm::vec3& target, float radius);

		/* 根据像素位置，获取EntityId */
		[[nodiscard]]
		int32_t PickingEntityByPixelPos(uint32_t x, uint32_t y) const;

	private:
		/* 当前导航手势：起手后锁定，直到其按键松开（期间不响应其他手势） */
		enum class NavGesture : uint8_t
		{
			None = 0,
			Orbit,		/* Alt+LMB：绕轨道中心旋转 */
			Pan,		/* MMB（可带 Alt）：沿视线平面平移 */
			ZoomDrag,	/* Alt+RMB：拖拽拉近拉远 */
			Fly,		/* RMB：鼠标转向 + WASD/QE 飞行 */
		};

		/* 构建内置的FrameGraph */
		bool ConstructRenderView(RenderView& render_view) override;

		void UpdateProjectionMatrix();
		void UpdateViewMatrix();
		void UpdateCameraDirections();

		/* 视口导航状态机：手势起手 / 保持 / 收尾，并派发本帧鼠标增量 */
		void UpdateNavigation(float delta_time, bool allow_new_gesture);
		/* 飞行：鼠标原地转向（枢轴跟到视线正前方）+ WASD/QE 沿视线平移 */
		void UpdateFly(const glm::vec2& mouse_delta, float delta_time);

		/* 轨道操作 */
		void OnMousePan(const glm::vec2& delta); // 整体平移
		void OnMouseRotate(const glm::vec2& delta); // 绕聚焦中心旋转
		void OnMouseZoom(float delta); // 拉远拉近（Alt+RMB 拖拽）

		[[nodiscard]]
		glm::vec2 PanSpeed() const;
		[[nodiscard]]
		float RotateSpeed() const;
		[[nodiscard]]
		float ZoomSpeed() const;

		/* 默认视角：轻微俯视 20°（正 = 向下看），打开即可看清地面网格 */
		float m_Pitch{ glm::radians(20.0f) }, m_Yaw{ 0.0f };
		/* 相机距离（轨道半径；位置 = 聚焦点 - 前向 * 距离，由它派生） */
		float m_Distance{ 10.0f };
		/* 相机移动速度（飞行） */
		float m_MoveSpeed{ 5.0f };

		/* 当前导航手势 */
		NavGesture m_ActiveGesture{ NavGesture::None };
		/* 是否允许起手新的导航手势（由场景层每帧授权） */
		bool m_IsNavigationAllowed{ false };
		/* 上一帧按键状态：起手要求“按下沿”，避免按住按键从其他窗口拖进视口时误起手 */
		bool m_WasAltPressed{ false };
		bool m_WasLmbPressed{ false };
		bool m_WasMmbPressed{ false };
		bool m_WasRmbPressed{ false };
		/* 是否需要更新变换矩阵 */
		bool m_IsDirty{ true };

		glm::vec3 m_FocalPoint{ 0.0f, 0.0f, 0.0f };
		/* 上一帧鼠标位置：增量逐帧维护（不在手势里也更新），起手不跳变 */
		glm::vec2 m_LastMousePosition{ 0.0f, 0.0f };

		/* 视口区域 */
		ViewportRegion m_ViewportRegion{};
	};
}
