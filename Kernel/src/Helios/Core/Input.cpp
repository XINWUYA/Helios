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
	}

	bool Input::IsKeyPressed(const KeyCode key)
	{
		GLFWwindow* window = CurrentInputWindow();
		if (window == nullptr)
			return false;

		auto state = glfwGetKey(window, static_cast<int32_t>(key));
		return state == GLFW_PRESS || state == GLFW_REPEAT;
	}

	bool Input::IsMouseButtonPressed(const MouseCode btn)
	{
		GLFWwindow* window = CurrentInputWindow();
		if (window == nullptr)
			return false;

		auto state = glfwGetMouseButton(window, static_cast<int32_t>(btn));
		return state == GLFW_PRESS;
	}

	glm::vec2 Input::GetMousePos()
	{
		GLFWwindow* window = CurrentInputWindow();
		if (window == nullptr)
			return glm::vec2(0.0f);

		double pos_x, pos_y;
		glfwGetCursorPos(window, &pos_x, &pos_y);

		return glm::vec2(static_cast<float>(pos_x), static_cast<float>(pos_y));
	}

	float Input::GetMouseX()
	{
		return GetMousePos().x;
	}

	float Input::GetMouseY()
	{
		return GetMousePos().y;
	}
}
