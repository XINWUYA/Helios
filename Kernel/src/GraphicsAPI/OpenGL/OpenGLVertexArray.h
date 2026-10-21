#pragma once
#include "Helios/VirtualDevice/DeviceVertexArray.h"
#include "Helios/VirtualDevice/DeviceShader.h"

namespace Helios
{
	class OpenGLVertexArray : public DeviceVertexArray
	{
	public:
		OpenGLVertexArray(const std::string& name);
		~OpenGLVertexArray() override;

		void Bind() const override;
		void Unbind() const override;

		void AddVertexBuffer(const SharedPtr<DeviceVertexBuffer>& vertex_buffer) override;
		void SetIndexBuffer(const SharedPtr<IndexBuffer>& index_buffer) override;

		const std::vector<SharedPtr<DeviceVertexBuffer>>& GetVertexBuffers() const override { return m_VertexBuffers; }
		const SharedPtr<IndexBuffer>& GetIndexBuffer() const override { return m_IndexBuffer; }
		uint32_t GetVertexCount() const override;

		/* 按着色器声明的属性位置配置属性指针：着色器要什么名字就绑到它给的 location 上，
		 * 布局里多出来的元素忽略。配置结果按指纹缓存，布局与着色器都没变时直接跳过。 */
		void ConfigureAttributes(const ShaderReflectionData& reflection);

	private:
		uint32_t m_VertexArrayId{ 0 };
		uint32_t m_VertexCount{ 0 };
		uint64_t m_AttributeFingerprint{ 0 };
		std::vector<SharedPtr<DeviceVertexBuffer>> m_VertexBuffers;
		SharedPtr<IndexBuffer> m_IndexBuffer;
	};
}

