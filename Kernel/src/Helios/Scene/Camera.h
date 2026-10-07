#pragma once
#include <glm/glm.hpp>
#include "Helios/Renderer/RenderView.h"
#include "SceneObject.h"

namespace Helios
{
	/* 投影类型 */
	enum class CameraProjectionType : uint8_t
	{
		Perspective = 0, // 透视
		Orthographic = 1 // 正交
	};

	/* 相机类 */
	class Camera : public SceneObject
	{
	public:
		Camera(CameraProjectionType type = CameraProjectionType::Perspective, float fov = 45.0f, float aspect_ratio = 1.778f, float near_clip = 0.1f, float far_clip = 1000.0f);
		virtual ~Camera() = default;

		/* 更新时间戳 */
		virtual void OnUpdate(float delta_time);

		/* 投影类型 */
		CameraProjectionType GetProjectionType() const { return m_ProjectionType; }
		void SetProjectionType(CameraProjectionType type);

		/* 渲染管线（前向 / 延迟）：状态在 RenderView 上（每个视图单独对应一套渲染组织），
		 * 这里转发保持"相机即其视图"的调用面（与 SetViewportRegion 的镜像惯例一致）。 */
		RenderPipeline GetRenderPipeline() const { return m_pRenderView->GetRenderPipeline(); }
		void SetRenderPipeline(RenderPipeline pipeline) { m_pRenderView->SetRenderPipeline(pipeline); }

		/* 相机位置（复用 SceneObject 基类存储） */
		void SetPosition(const glm::vec3& position) override;

		/* 根据世界变换矩阵更新相机位置、朝向和视图矩阵 */
		void SetTransform(const glm::mat4& transform) override;

		/* 相机旋转：基类以欧拉角(vec3, 弧度)表达，相机仅取绕 Z 轴的 roll 角（角度制） */
		glm::vec3 GetRotation() const override { return glm::vec3(0.0f, 0.0f, glm::radians(m_ZRoll)); }
		void SetRotation(const glm::vec3& rotation) override { SetRotation(glm::degrees(rotation.z)); }

		/* 相机旋转（绕 Z 轴，角度制）—— 相机专用接口 */
		float GetRotationZ() const { return m_ZRoll; }
		void SetRotation(float rotation);

		/* 相机方向 */
		const glm::vec3& GetUpDir() const { return m_UpDirection; }
		const glm::vec3& GetRightDir() const { return m_RightDirection; }
		const glm::vec3& GetForwardDir() const { return m_ForwardDirection; }

		/* 宽高比 */
		float GetAspectRatio() const { return m_AspectRatio; }
		void SetAspectRatio(float aspect_ratio) { m_AspectRatio = aspect_ratio; m_IsDirty = true; }

		/* Field of View */
		float GetFov() const { return m_Fov; }
		void SetFov(float fov) { m_Fov = fov; m_IsDirty = true; }

		/* 相机近平面 */
		float GetNearClip() const { return m_NearClip; }
		void SetNearClip(float near_clip) { m_NearClip = near_clip; m_IsDirty = true; }

		/* 相机原平面 */
		float GetFarClip() const { return m_FarClip; }
		void SetFarClip(float far_clip) { m_FarClip = far_clip; m_IsDirty = true; }

		/* 高度区域（正交相机） */
		float GetHeightSize() const { return m_HeightSize; }
		void SetHeightSize(float height_size) { m_HeightSize = height_size; m_IsDirty = true; }

		/* 矩阵信息 */
		const glm::mat4& GetViewMatrix() const { return m_ViewMatrix; }
		const glm::mat4& GetProjectionMatrix() const { return m_ProjectionMatrix; }
		glm::mat4 GetViewProjectionMatrix() const { return m_ProjectionMatrix * m_ViewMatrix; }

		/* 获取RenderView */
		RenderView* GetRenderView() const { return m_pRenderView.get(); }

	protected:
		/* 为当前相机构建 RenderView：在这里组织它的 FrameGraph，各相机 override 来用不同管线。返回
		 * true = 已自行组织；false = 用默认渲染图（跟管线选择一致）。调用时机 = 渲染视图执行、
		 * FrameGraph 重置之后 —— 渲染图每帧按当前视口重建，相机不用维护"要不要重建"的状态。 */
		virtual bool ConstructRenderView(RenderView& render_view) { return false; }
		/* 更新相机视图矩阵 */
		virtual void UpdateViewMatrix();
		/* 更新相机投影矩阵 */
		virtual void UpdateProjectionMatrix();

		/* 相机类型 */
		CameraProjectionType m_ProjectionType{ CameraProjectionType::Perspective };

		/* 相机基本参数 */
		float m_Fov{ 45.0f };
		float m_AspectRatio{ 1.778f };
		float m_NearClip{ 0.1f };
		float m_FarClip{ 1000.0f };

		/* 相机朝向 */
		glm::vec3 m_UpDirection{ 0.0f, 1.0f, 0.0f };
		glm::vec3 m_RightDirection{ 1.0f, 0.0f, 0.0f };
		glm::vec3 m_ForwardDirection{ 0.0f, 0.0f, -1.0f };

		/* 正交相机区域 */
		float m_HeightSize{ 10.0f };

		/* 相机旋转（绕 Z 轴 roll 角，角度制） */
		float m_ZRoll{ 0.0f };

		/* 相机矩阵 */
		glm::mat4 m_ViewMatrix{ glm::mat4(1.0f) };
		glm::mat4 m_ProjectionMatrix{ glm::mat4(1.0f) };

		/* 每个相机对应一个RenderView */
		UniquePtr<RenderView> m_pRenderView{ nullptr };
		bool m_IsDirty{ true };

		/* RenderView 在执行时需要回调 ConstructRenderView 组织渲染图 */
		friend class RenderView;
	};
}
