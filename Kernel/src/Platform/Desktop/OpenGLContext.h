#pragma once
#include "Helios/VirtualDevice/DeviceContext.h"

struct GLFWwindow;

namespace Helios
{
	class OpenGLContext final : public DeviceContext
	{
	public:
		OpenGLContext(GLFWwindow* window);
		~OpenGLContext() override = default;

		void Init() override;
		void SwapBuffers() override;

	private:
		GLFWwindow* m_pGLFWWindow{ nullptr };
	};
}
