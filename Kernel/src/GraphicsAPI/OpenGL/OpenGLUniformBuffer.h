#pragma once
#include "Helios/VirtualDevice/DeviceUniformBuffer.h"

namespace Helios
{
	/* 环形 UBO：每次 SetData 写进环里新的一段，再把绑定点指过去。
	 * 前面 draw 还在读的那段不会被覆盖，驱动不用等 GPU；转满一圈才整块换新存储 */
	class OpenGLUniformBuffer : public DeviceUniformBuffer
	{
	public:
		OpenGLUniformBuffer(uint32_t size, uint32_t binding_point);
		~OpenGLUniformBuffer() override;

		void SetData(const void* data, uint32_t size, uint32_t offset = 0) override;

		/* 绑定点在 SetData 时已指到最新那段 */
		void Bind() override {}

	private:
		uint32_t m_UniformBufferId{ 0 };
		uint32_t m_BindingPoint{ 0 };
		/* 一段的大小（按 UBO 偏移对齐取整）与段数 */
		uint32_t m_SlotSize{ 0 };
		uint32_t m_SlotCount{ 0 };
		uint32_t m_NextSlot{ 0 };
		/* 上一段的内容：局部更新（offset != 0）要在它的基础上改 */
		std::vector<uint8_t> m_Shadow;
	};
}
