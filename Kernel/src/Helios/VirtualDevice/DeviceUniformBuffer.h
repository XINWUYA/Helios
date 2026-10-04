#pragma once

namespace Helios
{
	/* DeviceUniformBuffer */
	class DeviceUniformBuffer
	{
	public:
		virtual ~DeviceUniformBuffer() = default;

		virtual void SetData(const void* data, uint32_t size, uint32_t offset = 0) = 0;

		/* 绑定到当前渲染管线的对应槽位。OpenGL 在构造时已绑定，可空实现；
		 * Metal 需要在 RenderCommandEncoder 上设置 setVertexBuffer/setFragmentBuffer。 */
		virtual void Bind() = 0;

		static SharedPtr<DeviceUniformBuffer> Create(uint32_t size, uint32_t binding_point);

	protected:
		DeviceUniformBuffer() = default;
	};
}


