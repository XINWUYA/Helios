#include "Pch.h"
#include "Helios/Reflection/ComponentRegistration.h"
#include "Helios/Scene/Components.h"

namespace Helios
{
	namespace
	{
		/* 投影类型决定哪些参数有意义：透视看 Fov，正交看 HeightSize */
		bool IsPerspectiveProjection(const void* component)
		{
			const auto& camera = static_cast<const CameraComponent*>(component)->m_Camera;
			return camera != nullptr && camera->GetProjectionType() == CameraProjectionType::Perspective;
		}

		bool IsOrthographicProjection(const void* component)
		{
			const auto& camera = static_cast<const CameraComponent*>(component)->m_Camera;
			return camera != nullptr && camera->GetProjectionType() == CameraProjectionType::Orthographic;
		}
	}

	void RegisterCameraComponent()
	{
		MakeRegistrar<CameraComponent>("Camera")
			.Field(&CameraComponent::m_IsPrimary, "IsPrimary", FieldType::Bool)
			.Field(&CameraComponent::m_IsFixedAspectRatio, "IsFixedAspectRatio", FieldType::Bool)
			/* 投影参数在嵌套的 Camera 对象里，经访问器读写 */
			.Field(MakeEnumAccessor<&CameraComponent::m_Camera,
				&Camera::GetProjectionType, &Camera::SetProjectionType>(),
				"ProjectionType", FieldType::Enum)
			.EnumOf<CameraProjectionType>()
			/* 渲染管线（前向 / 延迟）：同样经嵌套相机读写；缺省前向（旧场景兼容） */
			.Field(MakeEnumAccessor<&CameraComponent::m_Camera,
				&Camera::GetRenderPipeline, &Camera::SetRenderPipeline>(),
				"RenderPipeline", FieldType::Enum)
			.EnumOf<RenderPipeline>()
			/* 可见层掩码：与对象 LayerMask 相交非空才渲染 */
			.Field(MakeAccessor<&CameraComponent::m_Camera,
				&Camera::GetCullingMask, &Camera::SetCullingMask>(),
				"CullingMask", FieldType::LayerMask)
			.Field(MakeAccessor<&CameraComponent::m_Camera, &Camera::GetFov, &Camera::SetFov>(),
				"Fov", FieldType::Float)
			.When(FieldCondition{ 0, FieldType::Bool, ConditionOp::IsTrue, 0.0, &IsPerspectiveProjection })
			.Field(MakeAccessor<&CameraComponent::m_Camera, &Camera::GetHeightSize, &Camera::SetHeightSize>(),
				"HeightSize", FieldType::Float)
			.When(FieldCondition{ 0, FieldType::Bool, ConditionOp::IsTrue, 0.0, &IsOrthographicProjection })
			.Field(MakeAccessor<&CameraComponent::m_Camera, &Camera::GetNearClip, &Camera::SetNearClip>(),
				"NearClip", FieldType::Float)
			.SerializeName("Near")
			.Field(MakeAccessor<&CameraComponent::m_Camera, &Camera::GetFarClip, &Camera::SetFarClip>(),
				"FarClip", FieldType::Float)
			.SerializeName("Far")
			.Register();
	}
}
