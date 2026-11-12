/* 摇杆推相机、空白处拖拽转视角。两者共用主相机：
 * 摇杆的方向取自相机的水平朝向，转了视角，推摇杆就往新的方向走。
 * 速度与视角灵敏度可以从 HUD 的两个滑杆上调。 */
#include <cmath>
#include <cstdio>

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

			ApplySettings(scene);
			UpdateLook(scene, camera);
			UpdateMove(scene, camera, delta_time);
		}

	private:
		/* HUD 的两个滑杆：挂了就以滑杆为准，没挂时用面板里填的脚本参数 */
		void ApplySettings(const Helios::SharedPtr<Helios::Scene>& scene)
		{
			Helios::UI::UISystem& ui = scene->GetUISystem();
			m_Speed = ui.GetNumber(GetEntity(), "move_speed", m_Speed);
			m_LookSensitivity = ui.GetNumber(GetEntity(), "look_sensitivity", m_LookSensitivity);

			ShowValue(ui, "speed-value", m_Speed, 1, m_SpeedText);
			ShowValue(ui, "look-value", m_LookSensitivity, 2, m_LookText);
		}

		/* 值变了才写一次：每帧刷文本会连带重排 */
		void ShowValue(Helios::UI::UISystem& ui, const char* element_id, float value, int decimals,
			std::string& shown)
		{
			char buffer[32];
			std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, static_cast<double>(value));
			if (shown == buffer)
				return;

			shown = buffer;
			ui.SetText(GetEntity(), element_id, shown);
		}

		/* 摇杆平移：读画布的值，沿相机的水平朝向走 */
		void UpdateMove(const Helios::SharedPtr<Helios::Scene>& scene, const Helios::Entity camera,
			float delta_time)
		{
			Helios::UI::UISystem& ui = scene->GetUISystem();
			const glm::vec2 axis(ui.GetNumber(GetEntity(), "move_x", 0.0f),
				ui.GetNumber(GetEntity(), "move_y", 0.0f));
			const float strength = glm::min(glm::length(axis), 1.0f);
			if (strength < 0.01f)
				return;

			/* 朝向只看水平分量：抬头低头只改视线，不改变往前走是往哪走 */
			const glm::mat4 world = scene->GetWorldTransform(camera);
			glm::vec3 forward(-world[2].x, 0.0f, -world[2].z);
			if (glm::dot(forward, forward) < 1e-6f)
				return;

			forward = glm::normalize(forward);
			const glm::vec3 right(-forward.z, 0.0f, forward.x);

			/* 屏幕的 y 向下为正，摇杆往上推是往前走 */
			const glm::vec3 direction = right * axis.x - forward * axis.y;

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
			/* 欧拉角是（俯仰, 偏航, 翻滚）：左右拖改偏航，上下拖改俯仰并夹到接近垂直。
			 * 参数按「每个逻辑点转多少度」算，屏幕密度不同的设备手感一致 */
			const float step = glm::radians(m_LookSensitivity);
			transform.m_Rotation.y -= delta.x * step;
			constexpr float kPitchLimit = 1.4835f; /* 85 度 */
			transform.m_Rotation.x = glm::clamp(transform.m_Rotation.x - delta.y * step,
				-kPitchLimit, kPitchLimit);
		}

	public:
		/* 脚本参数：由扫描脚本字段的注册器按名字写入，必须是公开成员 */
		float m_Speed{ 9.0f };
		/* 视角灵敏度：每个逻辑点转多少度 */
		float m_LookSensitivity{ 0.3f };

	private:

		bool  m_Looking{ false };
		bool  m_WasPressed{ false };
		glm::vec2 m_LastMouse{ 0.0f };
		/* 上一次写进 HUD 的显示文本 */
		std::string m_SpeedText{};
		std::string m_LookText{};
	};
}

HELIOS_SCRIPT(CameraJoystick,
	HELIOS_SCRIPT_FIELD(&CameraJoystick::m_Speed, "Speed", Helios::FieldType::Float),
	HELIOS_SCRIPT_FIELD(&CameraJoystick::m_LookSensitivity, "LookSensitivity", Helios::FieldType::Float))
