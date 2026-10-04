#pragma once
#include "Helios/VirtualDevice/DeviceUniformBuffer.h"

namespace Helios
{
	class OpenGLUniformBuffer : public DeviceUniformBuffer
	{
	public:
		OpenGLUniformBuffer(uint32_t size, uint32_t binding_point);
		~OpenGLUniformBuffer() override;

		void SetData(const void* data, uint32_t size, uint32_t offset = 0) override;

		/* OpenGL 在构造时已通过 glBindBufferBase 绑定到 binding point */
		void Bind() override {}

	private:
		uint32_t m_UniformBufferId{ 0 };
	};
}
