#include "Pch.h"
#include "Light.h"

namespace Helios
{
	/* 开关投影：打开时把阴影配置一并备好 —— 属性面板的阴影参数字段、序列化与
	 * 渲染注册都从它取值，缺配置才创建；已有配置保持不变，关开关不销毁
	 * （再打开时之前调过的参数还在）。 */
	void Light::SetIsCastShadow(bool enable)
	{
		m_IsCastShadow = enable;
		if (enable && !m_pShadowMapInfo)
			m_pShadowMapInfo = CreateSharedPtr<ShadowMapInfo>();
	}

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
