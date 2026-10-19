/* 脚本系统样例：WASD 在平面上推着实体走，演示读输入、枚举参数与按帧累积位移。 */
#include "Helios/Core/Input.h"
#include "Helios/Script/ScriptRegistry.h"
#include "Helios/Scene/Components.h"

namespace
{
	class Mover : public Helios::Script
	{
	public:
		/* World：沿世界轴走；Local：沿实体自身朝向走 */
		enum class MoveSpace : int
		{
			World = 0,
			Local = 1,
		};

		void OnUpdate(float delta_time) override
		{
			if (!HasComponent<Helios::TransformComponent>())
				return;

			glm::vec3 direction{ 0.0f };
			if (Helios::Input::IsKeyPressed(Helios::Key::W)) direction.z -= 1.0f;
			if (Helios::Input::IsKeyPressed(Helios::Key::S)) direction.z += 1.0f;
			if (Helios::Input::IsKeyPressed(Helios::Key::A)) direction.x -= 1.0f;
			if (Helios::Input::IsKeyPressed(Helios::Key::D)) direction.x += 1.0f;

			if (glm::length(direction) < 0.0001f)
				return;

			direction = glm::normalize(direction);
			if (m_Space == MoveSpace::Local)
			{
				const glm::vec3& rotation = GetComponent<Helios::TransformComponent>().m_Rotation;
				direction = glm::rotate(glm::mat4(1.0f), rotation.y, glm::vec3(0.0f, 1.0f, 0.0f))
					* glm::vec4(direction, 0.0f);
			}

			auto& transform = GetComponent<Helios::TransformComponent>();
			transform.m_Position += direction * m_Speed * delta_time;
		}

		float     m_Speed{ 2.0f };
		MoveSpace m_Space{ MoveSpace::World };
	};
}

HELIOS_SCRIPT(Mover,
	HELIOS_SCRIPT_FIELD(&Mover::m_Speed, "Speed", Helios::FieldType::Float),
	HELIOS_SCRIPT_FIELD_ENUM(&Mover::m_Space, "Space", Mover::MoveSpace))
