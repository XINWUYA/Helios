#pragma once
#include "DeviceBuffer.h"


namespace Helios
{
	class HELIOS_API DeviceVertexArray
	{
	public:
		DeviceVertexArray() = default;
		virtual ~DeviceVertexArray() = default;

		virtual void Bind() const = 0;
		virtual void Unbind() const = 0;

		virtual void AddVertexBuffer(const SharedPtr<DeviceVertexBuffer>& vertex_buffer) = 0;
		virtual void SetIndexBuffer(const SharedPtr<IndexBuffer>& index_buffer) = 0;

		virtual const std::vector<SharedPtr<DeviceVertexBuffer>>& GetVertexBuffers() const = 0;
		virtual const SharedPtr<IndexBuffer>& GetIndexBuffer() const = 0;
		virtual uint32_t GetVertexCount() const = 0;

		static SharedPtr<DeviceVertexArray> Create(const std::string& name);
	};
}
