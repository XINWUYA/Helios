#include "Pch.h"
#include "Light.h"

namespace Helios
{
	/* 光传播方向 = 实体旋转下的 -Z 前向（与 SceneObject::GetTransform 同一套旋转表达） */
	glm::vec3 SpotLight::GetDirection() const
	{
		const glm::mat4 rotation = glm::toMat4(glm::quat(GetRotation()));
		return glm::normalize(glm::vec3(rotation * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
	}

	/* 创建指定类型光源 */
	SharedPtr<Light> Light::Create(LightType type)
	{
		switch (type)
		{
		case LightType::Directional:	return CreateSharedPtr<DirectionalLight>();
		case LightType::Point:			return CreateSharedPtr<PointLight>();
		case LightType::Spot:			return CreateSharedPtr<SpotLight>();
		case LightType::Area:			return CreateSharedPtr<AreaLight>();
		case LightType::Volume:			return CreateSharedPtr<VolumeLight>();
		}
		return nullptr;
	}
}
