#include "Pch.h"
#include "DeviceBuffer.h"
#include "Helios/Renderer/Renderer.h"
#include "GraphicsAPI/OpenGL/OpenGLBuffer.h"
#ifdef PLATFORM_MACOS
#include "GraphicsAPI/Metal/MetalBuffer.h"
#endif

namespace Helios
{
	static uint32_t GetBufferDataTypeSize(BufferDataType type)
	{
		switch (type)
		{
		case BufferDataType::Int:		return 4;
		case BufferDataType::Int2:		return 4 * 2;
		case BufferDataType::Int3:		return 4 * 3;
		case BufferDataType::Int4:		return 4 * 4;
		case BufferDataType::Bool:		return 1;
		case BufferDataType::Float:		return 4;
		case BufferDataType::Float2:	return 4 * 2;
		case BufferDataType::Float3:	return 4 * 3;
		case BufferDataType::Float4:	return 4 * 4;
		case BufferDataType::Mat3:		return 4 * 3 * 3;
		case BufferDataType::Mat4:		return 4 * 4 * 4;
		case BufferDataType::UByte4:	return 4;  // 4 bytes, normalized [0,255] -> [0.0,1.0]
		default:
			ASSERT(false, "Unknown BufferDataType!");
			return 0;
		}
	}

	BufferElement::BufferElement(const std::string& name, BufferDataType type, bool normalized)
		: Name(name), Type(type), Size(GetBufferDataTypeSize(type)), Offset(0), Normalized(normalized)
	{}

	uint32_t BufferElement::GetComponentCount() const
	{
		switch (Type)
		{
		case BufferDataType::Int:		return 1;
		case BufferDataType::Int2:		return 2;
		case BufferDataType::Int3:		return 3;
		case BufferDataType::Int4:		return 4;
		case BufferDataType::Bool:		return 1;
		case BufferDataType::Float:		return 1;
		case BufferDataType::Float2:	return 2;
		case BufferDataType::Float3:	return 3;
		case BufferDataType::Float4:	return 4;
		case BufferDataType::Mat3:		return 3; // Mat need process specially
		case BufferDataType::Mat4:		return 4; // Mat need process specially
		case BufferDataType::UByte4:	return 4;  // 4 components (RGBA)
		default: 
			ASSERT(false, "Unknown BufferDataType!");
			return 0;
		}
	}

	VertexBufferLayout::VertexBufferLayout(std::initializer_list<BufferElement> elements)
		: m_Elements(elements)
	{
		CalculateOffsetsAndStride();
	}

	void VertexBufferLayout::EmplaceElement(const BufferElement& element)
	{
		m_Elements.emplace_back(element);
		CalculateOffsetsAndStride();
	}

	void VertexBufferLayout::CalculateOffsetsAndStride()
	{
		m_Stride = 0;
		size_t offset = 0;
		for (auto& element : m_Elements)
		{
			element.Offset = offset;
			offset += element.Size;
			m_Stride += element.Size;
		}
	}

	SharedPtr<DeviceVertexBuffer> DeviceVertexBuffer::Create(const std::string& name, uint32_t size)
	{
		switch (Renderer::CurrentAPI())
		{
		case RenderAPI::None:
			CORE_LOG_ERROR("RenderAPI can't be None!");
			return nullptr;
		case RenderAPI::OpenGL:
			return CreateSharedPtr<OpenGLVertexBuffer>(name, size);
#ifdef PLATFORM_MACOS
		case RenderAPI::Metal:
			return CreateSharedPtr<MetalVertexBuffer>(name, size);
#endif
		default:
			CORE_LOG_ERROR("Unknown RenderAPI is unsupported!");
			return nullptr;
		}
	}

	SharedPtr<DeviceVertexBuffer> DeviceVertexBuffer::CreateShared(const std::string& name,
		const SharedPtr<DeviceStorageBuffer>& storage, uint32_t size)
	{
#ifndef __APPLE__
		/* GL 的缓冲名与用途无关：同一块显存直接当顶点缓冲用，VAO 只记缓冲名 */
		if (Renderer::CurrentAPI() == RenderAPI::OpenGL)
		{
			auto gl_storage = std::dynamic_pointer_cast<OpenGLStorageBuffer>(storage);
			if (gl_storage != nullptr && gl_storage->GetBufferID() != 0)
				return CreateSharedPtr<OpenGLVertexBuffer>(name, gl_storage->GetBufferID(), size);
		}
#endif
#ifdef PLATFORM_MACOS
		if (Renderer::CurrentAPI() == RenderAPI::Metal)
		{
			auto metal_storage = std::dynamic_pointer_cast<MetalStorageBuffer>(storage);
			if (metal_storage != nullptr && metal_storage->GetMetalBuffer() != nullptr)
				return CreateSharedPtr<MetalVertexBuffer>(name, metal_storage->GetMetalBuffer(), size);
		}
#endif
		return nullptr;
	}

	SharedPtr<DeviceVertexBuffer> DeviceVertexBuffer::Create(const std::string& name, const void* vertices, uint32_t size)
	{
		switch (Renderer::CurrentAPI())
		{
		case RenderAPI::None:
			CORE_LOG_ERROR("RenderAPI can't be None!");
			return nullptr;
		case RenderAPI::OpenGL:
			return CreateSharedPtr<OpenGLVertexBuffer>(name, vertices, size);
#ifdef PLATFORM_MACOS
		case RenderAPI::Metal:
			return CreateSharedPtr<MetalVertexBuffer>(name, vertices, size);
#endif
		default:
			CORE_LOG_ERROR("Unknown RenderAPI is unsupported!");
			return nullptr;
		}
	}

	SharedPtr<IndexBuffer> IndexBuffer::Create(const std::string& name, const void* indices, uint32_t count, IndexType type)
	{
		switch (Renderer::CurrentAPI())
		{
		case RenderAPI::None:
			CORE_LOG_ERROR("RenderAPI can't be None!");
			return nullptr;
		case RenderAPI::OpenGL:
			return CreateSharedPtr<OpenGLIndexBuffer>(name, indices, count, type);
#ifdef PLATFORM_MACOS
		case RenderAPI::Metal:
			return CreateSharedPtr<MetalIndexBuffer>(name, indices, count, type);
#endif
		default:
			CORE_LOG_ERROR("Unknown RenderAPI is unsupported!");
			return nullptr;
		}
	}

	SharedPtr<IndexBuffer> IndexBuffer::Create(const std::string& name, uint32_t count, IndexType type)
	{
		switch (Renderer::CurrentAPI())
		{
		case RenderAPI::None:
			CORE_LOG_ERROR("RenderAPI can't be None!");
			return nullptr;
		case RenderAPI::OpenGL:
			return CreateSharedPtr<OpenGLIndexBuffer>(name, count, type);
#ifdef PLATFORM_MACOS
		case RenderAPI::Metal:
			return CreateSharedPtr<MetalIndexBuffer>(name, count, type);
#endif
		default:
			CORE_LOG_ERROR("Unknown RenderAPI is unsupported!");
			return nullptr;
		}
	}
}

namespace Helios
{
	SharedPtr<DeviceStorageBuffer> DeviceStorageBuffer::Create(const std::string& name, const void* data, uint32_t size)
	{
		switch (Renderer::CurrentAPI())
		{
		case RenderAPI::None:
			CORE_LOG_ERROR("RenderAPI can't be None!");
			return nullptr;
		case RenderAPI::OpenGL:
#ifdef __APPLE__
			/* macOS 只有 GL 4.1，没有 SSBO */
			CORE_LOG_ERROR("Storage buffer is not supported by OpenGL 4.1 on macOS!");
			return nullptr;
#else
			return CreateSharedPtr<OpenGLStorageBuffer>(name, data, size);
#endif
#ifdef PLATFORM_MACOS
		case RenderAPI::Metal:
			return CreateSharedPtr<MetalStorageBuffer>(name, data, size);
#endif
		default:
			CORE_LOG_ERROR("Storage buffer is not supported by this render API!");
			return nullptr;
		}
	}

	SharedPtr<DeviceStorageBuffer> DeviceStorageBuffer::Create(const std::string& name, uint32_t size)
	{
		return Create(name, nullptr, size);
	}
}
