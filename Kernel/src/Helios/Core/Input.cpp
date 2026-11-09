#include "Pch.h"
#include "Input.h"
#include <GLFW/glfw3.h>
#include "Helios/Application/Application.h"

namespace Helios
{
	namespace
	{
		/* 拿本帧输入用的窗口：无窗口环境（headless 检查直接驱动相机这类）没有应用实例，
		 * 返回空 —— 各接口按"没按下 / 指针在原点"兜底 */
		GLFWwindow* CurrentInputWindow()
		{
			Application* application = Application::Instance();
			return (application != nullptr)
				? static_cast<GLFWwindow*>(application->GetWindow().GetNativeWindow())
				: nullptr;
		}

		/* 闸的当前状态：默认开着、没有画面区域 —— 没人调 SetGameInput 时一切照旧 */
		bool s_GameFocused = true;
		InputViewport s_GameViewport{};

		/* 闸是否放行：游戏画面有焦点，且应用窗口本身拿着系统焦点 */
		bool GameInputOpen()
		{
			if (!s_GameFocused)
				return false;

			GLFWwindow* window = CurrentInputWindow();
			return window != nullptr && glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0;
		}
	}

	void Input::SetGameInput(bool focused, const InputViewport& viewport)
	{
		s_GameFocused = focused;
		s_GameViewport = viewport;
	}

	bool Input::IsKeyPressedRaw(const KeyCode key)
	{
		GLFWwindow* window = CurrentInputWindow();
		if (window == nullptr)
			return false;

		auto state = glfwGetKey(window, static_cast<int32_t>(key));
		return state == GLFW_PRESS || state == GLFW_REPEAT;
	}

	bool Input::IsMouseButtonPressedRaw(const MouseCode btn)
	{
		GLFWwindow* window = CurrentInputWindow();
		if (window == nullptr)
			return false;

		auto state = glfwGetMouseButton(window, static_cast<int32_t>(btn));
		return state == GLFW_PRESS;
	}

	glm::vec2 Input::GetMousePosRaw()
	{
		GLFWwindow* window = CurrentInputWindow();
		if (window == nullptr)
			return glm::vec2(0.0f);

		double pos_x, pos_y;
		glfwGetCursorPos(window, &pos_x, &pos_y);

		return glm::vec2(static_cast<float>(pos_x), static_cast<float>(pos_y));
	}

	bool Input::IsKeyPressed(const KeyCode key)
	{
		return GameInputOpen() && IsKeyPressedRaw(key);
	}

	bool Input::IsMouseButtonPressed(const MouseCode btn)
	{
		return GameInputOpen() && IsMouseButtonPressedRaw(btn);
	}

	glm::vec2 Input::GetMousePos()
	{
		if (!GameInputOpen())
			return glm::vec2(0.0f);

		const glm::vec2 position = GetMousePosRaw();
		return (s_GameViewport.Size.x > 0.0f && s_GameViewport.Size.y > 0.0f)
			? position - s_GameViewport.Min : position;
	}

	float Input::GetMouseX()
	{
		return GetMousePos().x;
	}

	float Input::GetMouseY()
	{
		return GetMousePos().y;
	}

	glm::vec2 Input::GetMousePosNormalized()
	{
		if (!GameInputOpen() || s_GameViewport.Size.x <= 0.0f || s_GameViewport.Size.y <= 0.0f)
			return glm::vec2(0.0f);

		return (GetMousePosRaw() - s_GameViewport.Min) / s_GameViewport.Size;
	}
}
