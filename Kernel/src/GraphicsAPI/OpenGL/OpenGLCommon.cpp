#include "Pch.h"
#include "OpenGLCommon.h"

namespace Helios
{
	namespace GLState
	{
		namespace
		{
			GLuint g_Framebuffer = 0;
			GLuint g_Program = 0;
			uint32_t g_StateSerial = 1;
			std::unordered_map<uint64_t, BufferBinding> g_BufferBindings;
		}

		void BindFramebuffer(GLuint framebuffer)
		{
			glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
			g_Framebuffer = framebuffer;
			++g_StateSerial;
		}

		GLuint CurrentFramebuffer()
		{
			return g_Framebuffer;
		}

		void BindBufferRange(GLenum target, GLuint binding, const BufferBinding& range)
		{
			if (range.Buffer != 0 && range.Size > 0)
				glBindBufferRange(target, binding, range.Buffer, range.Offset, range.Size);
			else
				glBindBufferBase(target, binding, range.Buffer);
			g_BufferBindings[(static_cast<uint64_t>(target) << 32) | binding] = range;
		}

		uint32_t StateSerial()
		{
			return g_StateSerial;
		}

		BufferBinding CurrentBufferRange(GLenum target, GLuint binding)
		{
			const auto iter = g_BufferBindings.find((static_cast<uint64_t>(target) << 32) | binding);
			return iter != g_BufferBindings.end() ? iter->second : BufferBinding{};
		}

		void UseProgram(GLuint program)
		{
			if (g_Program == program)
				return;
			glUseProgram(program);
			g_Program = program;
			++g_StateSerial;
		}

		GLuint CurrentProgram()
		{
			return g_Program;
		}

		void ForgetProgram(GLuint program)
		{
			if (g_Program != program)
				return;
			glUseProgram(0);
			g_Program = 0;
			++g_StateSerial;
		}
	}

	/* 检查GLError信息 */
	void CheckGLError(const char* file, const char* func_name, uint32_t line) noexcept
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

		GLenum error = glGetError();
		if (error != GL_NO_ERROR)
		{
			CORE_LOG_ERROR("OpenGLError: {} in {}:{}, at: {}.", STRINGIFY(error), func_name, line, file);
		}
	}

	/* 检查FrameBuffer状态 */
	void CheckGLFrameBufferStatus(GLenum target, const char* func_name, uint32_t line) noexcept
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

		GLenum status = glCheckFramebufferStatus(target);
		if (status != GL_FRAMEBUFFER_COMPLETE)
		{
			const char* status_str = "UNKNOWN";
			switch (status)
			{
			case GL_FRAMEBUFFER_UNDEFINED: status_str = "GL_FRAMEBUFFER_UNDEFINED"; break;
			case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT: status_str = "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT"; break;
			case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT: status_str = "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT"; break;
			case GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER: status_str = "GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER"; break;
			case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER: status_str = "GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER"; break;
			case GL_FRAMEBUFFER_UNSUPPORTED: status_str = "GL_FRAMEBUFFER_UNSUPPORTED"; break;
			case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE: status_str = "GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE"; break;
			case GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS: status_str = "GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS"; break;
			}
			CORE_LOG_ERROR("OpenGLFrameBufferError: {} (0x{:X}) in {}():{}.", status_str, status, func_name, line);
		}
	}}
