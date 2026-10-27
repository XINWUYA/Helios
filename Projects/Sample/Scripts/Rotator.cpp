/* 脚本系统样例：绕自身 Y 轴匀速转。
 * 改 Speed 面板里立刻生效（进 Runtime 时灌进实例），不用重编引擎。 */
#include "Helios/Script/ScriptRegistry.h"
#include "Helios/Scene/Components.h"

namespace
{
	class Rotator : public Helios::Script
	{
	public:
		void OnUpdate(float delta_time) override
		{
			if (!HasComponent<Helios::TransformComponent>())
				return;

			auto& transform = GetComponent<Helios::TransformComponent>();
			transform.m_Rotation.y += glm::radians(m_Speed) * delta_time;
		}

		/* 可编辑参数：字段宏要取成员地址，所以对外可见 */
		float     m_Speed{ 90.0f };
		glm::vec3 m_Axis{ 0.0f, 1.0f, 0.0f };
	};
}

HELIOS_SCRIPT(Rotator,
	HELIOS_SCRIPT_FIELD(&Rotator::m_Speed, "Speed", Helios::FieldType::Float),
	HELIOS_SCRIPT_FIELD(&Rotator::m_Axis, "Axis", Helios::FieldType::Vec3))
