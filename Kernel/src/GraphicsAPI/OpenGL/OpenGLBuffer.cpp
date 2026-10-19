#include "Pch.h"
#include "OpenGLBuffer.h"
#include <glad/glad.h>

namespace Helios
{
	OpenGLVertexBuffer::OpenGLVertexBuffer(const std::string& name, uint32_t size)
		: m_DataSize(size)
	{
		PROFILE_FUNCTION();

#ifdef __APPLE__
		// macOS: Use traditional functions (OpenGL 4.1 compatible)
		glGenBuffers(1, &m_VertexBufferId);
		glBindBuffer(GL_ARRAY_BUFFER, m_VertexBufferId);
		glBufferData(GL_ARRAY_BUFFER, size, nullptr, GL_DYNAMIC_DRAW);
#else
		// Windows/Linux: Use modern DSA functions (OpenGL 4.5+)
		glCreateBuffers(1, &m_VertexBufferId);
		glNamedBufferData(m_VertexBufferId, size, nullptr, GL_DYNAMIC_DRAW);
#ifdef HELIOS_DEBUG
		glObjectLabel(GL_BUFFER, m_VertexBufferId, -1, name.c_str());
#endif
#endif
	}

	OpenGLVertexBuffer::OpenGLVertexBuffer(const std::string& name, const void* vertices, uint32_t size)
		: m_DataSize(size)
	{
		PROFILE_FUNCTION();

#ifdef __APPLE__
		// macOS: Use traditional functions (OpenGL 4.1 compatible)
		glGenBuffers(1, &m_VertexBufferId);
		glBindBuffer(GL_ARRAY_BUFFER, m_VertexBufferId);
		glBufferData(GL_ARRAY_BUFFER, size, vertices, GL_STATIC_DRAW);
#else
		// Windows/Linux: Use modern DSA functions (OpenGL 4.5+)
		glCreateBuffers(1, &m_VertexBufferId);
		glNamedBufferData(m_VertexBufferId, size, vertices, GL_STATIC_DRAW);
#ifdef HELIOS_DEBUG
		glObjectLabel(GL_BUFFER, m_VertexBufferId, -1, name.c_str());
#endif
#endif
	}

	/* 复用外部 GL 缓冲：名字归传入方所有，析构时不删 */
	OpenGLVertexBuffer::OpenGLVertexBuffer(const std::string& name, uint32_t buffer_id, uint32_t size)
		: m_VertexBufferId(buffer_id), m_DataSize(size), m_OwnsBuffer(false)
	{
		PROFILE_FUNCTION();

		(void)name;
	}

	OpenGLVertexBuffer::~OpenGLVertexBuffer()
	{
		PROFILE_FUNCTION();

		if (m_OwnsBuffer)
			glDeleteBuffers(1, &m_VertexBufferId);
	}

	/* 绑定顶点数据 */
	void OpenGLVertexBuffer::Bind() const
	{
		PROFILE_FUNCTION();

		glBindBuffer(GL_ARRAY_BUFFER, m_VertexBufferId);
	}

	/* 解绑顶点数据 */
	void OpenGLVertexBuffer::Unbind() const
	{
		PROFILE_FUNCTION();

		glBindBuffer(GL_ARRAY_BUFFER, 0);
	}

	/* 设置顶点数据 */
	void OpenGLVertexBuffer::SetData(const void* data, uint32_t size)
	{
		PROFILE_FUNCTION();

#ifdef __APPLE__
		// macOS: Use traditional functions
		glBindBuffer(GL_ARRAY_BUFFER, m_VertexBufferId);
		glBufferSubData(GL_ARRAY_BUFFER, 0, size, data);
#else
		// Windows/Linux: Use modern DSA function
		glNamedBufferSubData(m_VertexBufferId, 0, size, data);
#endif

		m_DataSize = size;
	}

	/* 获取顶点数量 */
	uint32_t OpenGLVertexBuffer::GetVertexCount() const
	{
		PROFILE_FUNCTION();

		ASSERT(!(m_DataSize % m_Layout.GetStride()), "VertexBuffer data is not complete!");
		return m_DataSize / m_Layout.GetStride();
	}

	OpenGLIndexBuffer::OpenGLIndexBuffer(const std::string& name, const void* indices, uint32_t count, IndexType type)
		: m_Count(count), m_IndexType(type)
	{
		PROFILE_FUNCTION();

		const uint32_t index_size = (type == IndexType::UInt16) ? sizeof(uint16_t) : sizeof(uint32_t);
		const uint32_t size_in_bytes = count * index_size;

#ifdef __APPLE__
		// macOS: Use traditional functions (OpenGL 4.1 compatible)
		glGenBuffers(1, &m_IndexBufferId);
		// GL_ELEMENT_ARRAY_BUFFER is not valid without an actively bound VAO
		// Binding with GL_ARRAY_BUFFER allows the data to be loaded regardless of VAO state.
		glBindBuffer(GL_ARRAY_BUFFER, m_IndexBufferId);
		glBufferData(GL_ARRAY_BUFFER, size_in_bytes, indices, GL_STATIC_DRAW);
#else
		// Windows/Linux: Use modern DSA functions (OpenGL 4.5+)
		glCreateBuffers(1, &m_IndexBufferId);
		glNamedBufferData(m_IndexBufferId, size_in_bytes, indices, GL_STATIC_DRAW);
#ifdef HELIOS_DEBUG
		glObjectLabel(GL_BUFFER, m_IndexBufferId, -1, name.c_str());
#endif
#endif
	}

	/* 预分配空索引缓冲区（用于后续 SetData 动态更新，如ImGui） */
	OpenGLIndexBuffer::OpenGLIndexBuffer(const std::string& name, uint32_t count, IndexType type)
		: m_Count(count), m_IndexType(type)
	{
		PROFILE_FUNCTION();

		const uint32_t index_size = (type == IndexType::UInt16) ? sizeof(uint16_t) : sizeof(uint32_t);
		const uint32_t size_in_bytes = count * index_size;

#ifdef __APPLE__
		glGenBuffers(1, &m_IndexBufferId);
		// 与其他构造保持一致，绑定到 GL_ARRAY_BUFFER 避免对VAO的依赖
		glBindBuffer(GL_ARRAY_BUFFER, m_IndexBufferId);
		glBufferData(GL_ARRAY_BUFFER, size_in_bytes, nullptr, GL_DYNAMIC_DRAW);
#else
		glCreateBuffers(1, &m_IndexBufferId);
		glNamedBufferData(m_IndexBufferId, size_in_bytes, nullptr, GL_DYNAMIC_DRAW);
#ifdef HELIOS_DEBUG
		glObjectLabel(GL_BUFFER, m_IndexBufferId, -1, name.c_str());
#endif
#endif
	}

	OpenGLIndexBuffer::~OpenGLIndexBuffer()
	{
		PROFILE_FUNCTION();

		glDeleteBuffers(1, &m_IndexBufferId);
	}

	/* 绑定索引数据 */
	void OpenGLIndexBuffer::Bind() const
	{
		PROFILE_FUNCTION();

		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_IndexBufferId);
	}

	/* 解绑索引数据 */
	void OpenGLIndexBuffer::Unbind() const
	{
		PROFILE_FUNCTION();

		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	}

	/* 更新索引数据 */
	void OpenGLIndexBuffer::SetData(const void* data, uint32_t size)
	{
		PROFILE_FUNCTION();

#ifdef __APPLE__
		// macOS: Use traditional functions
		// 与构造时保持一致，先用 GL_ARRAY_BUFFER 更新数据（不依赖VAO绑定）
		glBindBuffer(GL_ARRAY_BUFFER, m_IndexBufferId);
		glBufferSubData(GL_ARRAY_BUFFER, 0, size, data);
#else
		// Windows/Linux: Use modern DSA function
		glNamedBufferSubData(m_IndexBufferId, 0, size, data);
#endif

		/* 根据新写入的字节数重新计算索引数量 */
		const uint32_t index_size = (m_IndexType == IndexType::UInt16) ? sizeof(uint16_t) : sizeof(uint32_t);
		m_Count = size / index_size;
	}

#ifndef __APPLE__
	OpenGLStorageBuffer::OpenGLStorageBuffer(const std::string& name, const void* data, uint32_t size)
		: m_Size(size)
	{
		PROFILE_FUNCTION();

		glCreateBuffers(1, &m_BufferID);
		glNamedBufferData(m_BufferID, size, data, GL_DYNAMIC_DRAW);
#ifdef HELIOS_DEBUG
		glObjectLabel(GL_BUFFER, m_BufferID, -1, name.c_str());
#endif
	}

	OpenGLStorageBuffer::~OpenGLStorageBuffer()
	{
		PROFILE_FUNCTION();

		glDeleteBuffers(1, &m_BufferID);
	}

	void OpenGLStorageBuffer::SetData(const void* data, uint32_t size)
	{
		PROFILE_FUNCTION();

		if (m_BufferID == 0 || data == nullptr || size > m_Size)
			return;

		glNamedBufferSubData(m_BufferID, 0, size, data);
	}

	/* 计算着色器写完之后要在 CPU 侧看结果就得回读：映射接口没有常驻指针，
	 * 每次读进镜像再返回（这条路径只在检查 / 工具里用） */
	void* OpenGLStorageBuffer::GetMappedData()
	{
		PROFILE_FUNCTION();

		if (m_BufferID == 0 || m_Size == 0)
			return nullptr;

		m_Shadow.resize(m_Size);
		glGetNamedBufferSubData(m_BufferID, 0, m_Size, m_Shadow.data());
		return m_Shadow.data();
	}
#endif
}
