#pragma once
#include <glm/glm.hpp>
#include "KeyCodes.h"
#include "MouseCodes.h"

namespace Helios
{
	class HELIOS_API Input
	{
	public:
		static bool IsKeyPressed(const KeyCode key);

		static bool IsMouseButtonPressed(const MouseCode btn);
		static glm::vec2 GetMousePos();
		static float GetMouseX();
		static float GetMouseY();
	};
}