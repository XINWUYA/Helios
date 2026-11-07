#include "Pch.h"
#include "OpenGLUniformBuffer.h"
#include "OpenGLCommon.h"
#include <glad/glad.h>

namespace Helios
{
	namespace
	{
		/* 环的总大小：Object 块一帧要写上千次，256 字节一段时 1MB 能放 4096 段 */
		constexpr uint32_t kRingBytes = 1u << 20;
	}

	OpenGLUniformBuffer::OpenGLUniformBuffer(uint32_t size, uint32_t binding_point)
		: m_BindingPoint(binding_point)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

		GLint alignment = 256;
		glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &alignment);
		alignment = std::max(alignment, 1);
		m_SlotSize = (size + alignment - 1) / alignment * alignment;
		m_SlotCount = std::max(1u, kRingBytes / m_SlotSize);
		m_Shadow.assign(size, 0);

		glGenBuffers(1, &m_UniformBufferId);
		glBindBuffer(GL_UNIFORM_BUFFER, m_UniformBufferId);
		glBufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(m_SlotSize) * m_SlotCount, nullptr, GL_DYNAMIC_DRAW);
		GLState::BindBufferRange(GL_UNIFORM_BUFFER, m_BindingPoint,
			{ m_UniformBufferId, 0, static_cast<GLsizeiptr>(m_Shadow.size()) });
	}

	OpenGLUniformBuffer::~OpenGLUniformBuffer()
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

		glDeleteBuffers(1, &m_UniformBufferId);
	}

	void OpenGLUniformBuffer::SetData(const void* data, uint32_t size, uint32_t offset)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

		if (data == nullptr || offset + size > m_Shadow.size())
			return;
		std::memcpy(m_Shadow.data() + offset, data, size);

		/* 转满一圈：整块孤立换新存储，旧存储等 GPU 用完再由驱动回收 */
		glBindBuffer(GL_UNIFORM_BUFFER, m_UniformBufferId);
		if (m_NextSlot == m_SlotCount)
		{
			glBufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(m_SlotSize) * m_SlotCount, nullptr, GL_DYNAMIC_DRAW);
			m_NextSlot = 0;
		}

		const GLintptr slot_offset = static_cast<GLintptr>(m_NextSlot) * m_SlotSize;
		glBufferSubData(GL_UNIFORM_BUFFER, slot_offset, static_cast<GLsizeiptr>(m_Shadow.size()), m_Shadow.data());
		GLState::BindBufferRange(GL_UNIFORM_BUFFER, m_BindingPoint,
			{ m_UniformBufferId, slot_offset, static_cast<GLsizeiptr>(m_Shadow.size()) });
		++m_NextSlot;
	}
}
