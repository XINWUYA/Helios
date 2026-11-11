#pragma once

#include "Helios/Common/Platform.h"

#if HELIOS_PLATFORM_DESKTOP

#include <glm/glm.hpp>
#include <string>
#include <GLFW/glfw3.h>

/* 桌面窗口共用的平台服务：输入轮询与剪贴板，都是 GLFW 的封装。
 * OpenGL 与 Metal 窗口都转到这里，改一处两边都生效。 */

namespace Helios::Desktop
{
	inline bool IsKeyPressed(GLFWwindow* window, int key)
	{
		if (window == nullptr)
			return false;

		const int state = glfwGetKey(window, key);
		return state == GLFW_PRESS || state == GLFW_REPEAT;
	}

	inline bool IsPointerButtonPressed(GLFWwindow* window, int button)
	{
		if (window == nullptr)
			return false;

		return glfwGetMouseButton(window, button) == GLFW_PRESS;
	}

	/* 窗口坐标（左上角原点，逻辑点），与 ImGui 的鼠标坐标同一空间 */
	inline glm::vec2 PointerPosition(GLFWwindow* window)
	{
		if (window == nullptr)
			return glm::vec2(0.0f);

		double x = 0.0;
		double y = 0.0;
		glfwGetCursorPos(window, &x, &y);
		return glm::vec2(static_cast<float>(x), static_cast<float>(y));
	}

	inline bool HasFocus(GLFWwindow* window)
	{
		return window != nullptr && glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0;
	}

	inline bool SetClipboardText(GLFWwindow* window, const std::string& text)
	{
		if (window == nullptr)
			return false;

		glfwSetClipboardString(window, text.c_str());
		return true;
	}

	/* 剪贴板为空或者系统不给读时返回 false，调用方回落到进程内实现 */
	inline bool GetClipboardText(GLFWwindow* window, std::string& text)
	{
		if (window == nullptr)
			return false;

		const char* clipboard = glfwGetClipboardString(window);
		if (clipboard == nullptr)
			return false;

		text = clipboard;
		return true;
	}
}

#endif
