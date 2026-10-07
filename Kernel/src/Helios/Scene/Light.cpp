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

	/* 光向推导的统一形式：旋转作用下的规范朝向轴。
	 * 聚光的规范轴 = -Z 前向；方向光的规范轴 = -Y（灯光图标的基准朝向，
	 * 与"单位旋转 = 光从头顶直下"的默认光向一致）。 */
	static glm::vec3 RotatedLightDirection(const glm::vec3& rotation, const glm::vec3& canonical_axis)
	{
		const glm::mat4 rotation_mat = glm::toMat4(glm::quat(rotation));
		return glm::normalize(glm::vec3(rotation_mat * glm::vec4(canonical_axis, 0.0f)));
	}

	/* 光传播方向 = 实体旋转下的 -Z 前向（与 SceneObject::GetTransform 同一套旋转表达） */
	glm::vec3 SpotLight::GetDirection() const
	{
		return RotatedLightDirection(GetRotation(), glm::vec3(0.0f, 0.0f, -1.0f));
	}

	/* 方向光的实体旋转是光向的输入：旋转一变就把方向重推一遍。
	 * 旋转未变时保持现状 —— 直接 SetDirection 的显式设定不被每帧的变换同步覆盖。 */
	void DirectionalLight::SetTransform(const glm::mat4& transform)
	{
		const glm::vec3 previous_rotation = GetRotation();
		Light::SetTransform(transform);

		if (GetRotation() != previous_rotation)
			m_Direciton = RotatedLightDirection(GetRotation(), glm::vec3(0.0f, -1.0f, 0.0f));
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
