#include "Pch.h"
#include "OpenGLContext.h"
#include <cstring>
#include <GLFW/glfw3.h>
#include <glad/glad.h>

namespace Helios
{
	namespace
	{
		/* glClipControl / glClipControlEXT 签名相同：void(GLenum origin, GLenum depth)。
		 * 运行时从 GL 加载器动态获取，不依赖编译期 glad 是否声明该符号。 */
		using PFNGLCLIPCONTROL = void (*)(GLenum origin, GLenum depth);

		/* EXT 版本与核心版本的裁剪空间枚举取值相同，桌面 / GLES 可共用。 */
#ifndef GL_LOWER_LEFT_EXT
#define GL_LOWER_LEFT_EXT 0x8CA1
#endif
#ifndef GL_ZERO_TO_ONE_EXT
#define GL_ZERO_TO_ONE_EXT 0x935F
#endif

		/* 运行时判定当前上下文是否为 OpenGL ES（读 GL_VERSION 前缀，与所用加载器无关）。 */
		bool IsOpenGLESContext()
		{
			const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
			return version != nullptr && std::strncmp(version, "OpenGL ES", 9) == 0;
		}
	}

	OpenGLContext::OpenGLContext(GLFWwindow* window)
		: m_pGLFWWindow(window)
	{
		ASSERT(window, "GLFWwindow is nullptr!");
	}

	void OpenGLContext::Init()
	{
		PROFILE_FUNCTION();

		glfwMakeContextCurrent(m_pGLFWWindow);

		// 初始化Glad
		int status = gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
		ASSERT(status, "Failed to init Glad!");

		CORE_LOG_INFO("Using OpenGL:");
		CORE_LOG_INFO("    Vendor: {0}", glGetString(GL_VENDOR));
		CORE_LOG_INFO("    Renderer: {0}", glGetString(GL_RENDERER));
		CORE_LOG_INFO("    Version: {0}", glGetString(GL_VERSION));

		const bool is_es = IsOpenGLESContext();

		if (is_es)
		{
			/* GLES 门槛：EXT_clip_control 需要 ES 3.0+ */
			ASSERT((GLVersion.major >= 3),
				"OpenGL ES Version is too old(need >= 3.0).");
		}
		else
		{
#ifdef __APPLE__
			// macOS only supports OpenGL 4.1
			ASSERT((GLVersion.major > 4 || (GLVersion.major == 4 && GLVersion.minor >= 1)),
				"OpenGL Version is too old(need >= 4.1 on macOS).");
#else
			ASSERT((GLVersion.major > 4 || (GLVersion.major == 4 && GLVersion.minor >= 5)),
				"OpenGL Version is too old(need >= 4.5).");
#endif
		}

		/* ZO + Reversed-Z 深度约定（跟 Metal 对齐）：裁剪空间 z 从 [-1,1] 切到 [0,1]，y 保持
		 * GL_LOWER_LEFT。桌面 GL 用 glClipControl(4.5+)、GLES 用 glClipControlEXT（签名一样，运行时
		 * 从加载器拿；取不到就显式报错）。 */
		const char* clip_control_name = is_es ? "glClipControlEXT" : "glClipControl";
		auto clip_control = reinterpret_cast<PFNGLCLIPCONTROL>(glfwGetProcAddress(clip_control_name));
		if (clip_control)
		{
			clip_control(GL_LOWER_LEFT_EXT, GL_ZERO_TO_ONE_EXT);
		}
		else
		{
			CORE_LOG_ERROR("{} is unavailable; the ZO/reversed-Z depth convention "
				"cannot be applied, rendering will be incorrect.", clip_control_name);
		}

		/* Reversed-Z：深度清零值为 0（远平面）。用 glClearDepthf：桌面 GL 4.1+ 与
		 * GLES 2.0+ 均可用，而 glClearDepth 在 GLES / ANGLE 上不导出。 */
		glClearDepthf(0.0f);


		/* 创建DebugOutput回调 */
#ifdef HELIOS_DEBUG
		int flags;
		glGetIntegerv(GL_CONTEXT_FLAGS, &flags);
		if (flags & GL_CONTEXT_FLAG_DEBUG_BIT)
		{
			glEnable(GL_DEBUG_OUTPUT);
			glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
			glDebugMessageCallback([](GLenum source, GLenum type, unsigned int id, GLenum severity, GLsizei length, const char* message, const void* user_params)
				{
					if (id == 131169 || id == 131185 || id == 131218 || id == 131204)  /* ignore these non-significant error codes */
						return;

					switch (severity)
					{
					case GL_DEBUG_SEVERITY_HIGH:
						CORE_LOG_CRITICAL("OpenGL DebugOutput: {} from Source({}), Type({}), Id({}).", message, STRINGIFY(source), STRINGIFY(type), id);
						break;
					case GL_DEBUG_SEVERITY_MEDIUM:
						CORE_LOG_ERROR("OpenGL DebugOutput: {} from Source({}), Type({}), Id({}).", message, STRINGIFY(source), STRINGIFY(type), id);
						break;
					case GL_DEBUG_SEVERITY_LOW:
						CORE_LOG_WARN("OpenGL DebugOutput: {} from Source({}), Type({}), Id({}).", message, STRINGIFY(source), STRINGIFY(type), id);
						break;
					case GL_DEBUG_SEVERITY_NOTIFICATION:
						CORE_LOG_INFO("OpenGL DebugOutput: {} from Source({}), Type({}), Id({}).", message, STRINGIFY(source), STRINGIFY(type), id);
						break;
					default:
						break;
					}
				}, nullptr);
			//glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_LOW, 0, nullptr, GL_TRUE);
			glDebugMessageControl(GL_DONT_CARE, GL_DEBUG_TYPE_ERROR, GL_DEBUG_SEVERITY_LOW, 0, nullptr, GL_TRUE);
		}
#endif
	}

	void OpenGLContext::SwapBuffers()
	{
		PROFILE_FUNCTION();

		glfwSwapBuffers(m_pGLFWWindow);
	}
}
