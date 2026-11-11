/* 摇杆推相机、空白处拖拽转视角。两者共用主相机：
 * 摇杆值来自运行时的画布控件，视角由鼠标拖拽直接改相机的欧拉角。 */
#include <cmath>

#include <glm/glm.hpp>

#include "Helios/Core/Input.h"
#include "Helios/Scene/Components.h"
#include "Helios/Script/ScriptRegistry.h"
#include "Helios/UI/UISystem.h"

namespace
{
	class CameraJoystick : public Helios::Script
	{
	public:
		void OnUpdate(float delta_time) override
		{
			if (!HasComponent<Helios::CanvasComponent>())
				return;

			const Helios::SharedPtr<Helios::Scene> scene = GetScene();
			if (!scene)
				return;

			const Helios::Entity camera = scene->GetPrimaryCameraEntity();
			if (!camera || !camera.HasComponent<Helios::TransformComponent>())
				return;

			UpdateLook(scene, camera);
			UpdateMove(scene, camera, delta_time);
		}

	private:
		/* 摇杆平移：读画布的值，沿相机朝向在水平面走 */
		void UpdateMove(const Helios::SharedPtr<Helios::Scene>& scene, const Helios::Entity camera,
			float delta_time)
		{
			Helios::UI::UISystem& ui = scene->GetUISystem();
			const glm::vec2 axis(ui.GetNumber(GetEntity(), "move_x", 0.0f),
				ui.GetNumber(GetEntity(), "move_y", 0.0f));
			const float strength = glm::min(glm::length(axis), 1.0f);
			if (strength < 0.01f)
				return;

			const glm::mat4 world = scene->GetWorldTransform(camera);
			const glm::vec3 forward = glm::normalize(glm::vec3(-world[2].x, 0.0f, -world[2].z));
			const glm::vec3 right = glm::normalize(glm::vec3(world[0].x, 0.0f, world[0].z));

			/* 屏幕的 y 向下为正，摇杆往上推是往前走 */
			const glm::vec3 direction = right * axis.x - forward * axis.y;
			if (glm::dot(direction, direction) < 1e-6f)
				return;

			Helios::TransformComponent& transform = camera.GetComponent<Helios::TransformComponent>();
			transform.m_Position += glm::normalize(direction) * (m_Speed * strength * delta_time);
		}

		/* 拖拽转视角：按下时指针落在 UI 控件上（摇杆、按钮、输入框）就整段不转，
		 * 空白处才转。控件的悬停态一直跟着指针走，按下当帧就能定下来 */
		void UpdateLook(const Helios::SharedPtr<Helios::Scene>& scene, const Helios::Entity camera)
		{
			const glm::vec2 mouse = Helios::Input::GetMousePos();
			const bool pressed = Helios::Input::IsMouseButtonPressed(Helios::Mouse::Button0);

			if (pressed && !m_WasPressed)
			{
				m_Looking = !scene->GetUISystem().IsPointerOverControl();
				m_LastMouse = mouse;
			}
			m_WasPressed = pressed;

			if (!pressed || !m_Looking)
			{
				if (!pressed)
					m_Looking = false;
				m_LastMouse = mouse;
				return;
			}

			const glm::vec2 delta = mouse - m_LastMouse;
			m_LastMouse = mouse;
			if (std::abs(delta.x) < 0.01f && std::abs(delta.y) < 0.01f)
				return;

			Helios::TransformComponent& transform = camera.GetComponent<Helios::TransformComponent>();
			/* 欧拉角是（俯仰, 偏航, 翻滚）：左右拖改偏航，上下拖改俯仰并夹到接近垂直 */
			transform.m_Rotation.y -= delta.x * m_LookSensitivity;
			constexpr float kPitchLimit = 1.5533f; /* 89 度 */
			transform.m_Rotation.x = glm::clamp(transform.m_Rotation.x - delta.y * m_LookSensitivity,
				-kPitchLimit, kPitchLimit);
		}

	public:
		/* 脚本参数：由扫描脚本字段的注册器按名字写入，必须是公开成员 */
		float m_Speed{ 9.0f };
		float m_LookSensitivity{ 0.12f };

	private:

		bool  m_Looking{ false };
		bool  m_WasPressed{ false };
		glm::vec2 m_LastMouse{ 0.0f };
	};
}

HELIOS_SCRIPT(CameraJoystick,
	HELIOS_SCRIPT_FIELD(&CameraJoystick::m_Speed, "Speed", Helios::FieldType::Float),
	HELIOS_SCRIPT_FIELD(&CameraJoystick::m_LookSensitivity, "LookSensitivity", Helios::FieldType::Float))
