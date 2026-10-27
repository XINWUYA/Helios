/* 脚本系统样例：朝场景里叫 m_TargetName 的实体移动，演示按名字找实体与用步长推进。 */
#include "Helios/Script/ScriptRegistry.h"
#include "Helios/Scene/Components.h"

namespace
{
	class Follower : public Helios::Script
	{
	public:
		void OnUpdate(float delta_time) override
		{
			if (!HasComponent<Helios::TransformComponent>())
				return;

			const Helios::Entity target = FindEntity(m_Target);
			if (!target || !target.HasComponent<Helios::TransformComponent>())
				return;

			auto& self = GetComponent<Helios::TransformComponent>();
			const auto& goal = target.GetComponent<Helios::TransformComponent>();

			const glm::vec3 offset = goal.m_Position - self.m_Position;
			const float distance = glm::length(offset);
			if (distance < 0.0001f)
				return;

			/* 一帧走不完就停在目标上，别越过再折返 */
			const float step = glm::min(m_Speed * delta_time, distance);
			self.m_Position += (offset / distance) * step;
		}

		/* 目标实体：面板上直接把层级里的节点拖进来，存档存名字 */
		std::string m_Target{ "Target" };
		float       m_Speed{ 2.0f };
	};
}

HELIOS_SCRIPT(Follower,
	HELIOS_SCRIPT_FIELD_ENTITY(&Follower::m_Target, "Target"),
	HELIOS_SCRIPT_FIELD(&Follower::m_Speed, "Speed", Helios::FieldType::Float))
