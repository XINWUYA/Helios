#include "Pch.h"
#include "OpenGLVertexArray.h"

#include <glad/glad.h>

namespace Helios
{
	OpenGLVertexArray::OpenGLVertexArray(const std::string& name)
	{
		PROFILE_FUNCTION();

#ifdef __APPLE__
		// macOS: Use traditional function (OpenGL 4.1 compatible)
		glGenVertexArrays(1, &m_VertexArrayId);
#else
		// Windows/Linux: Use modern DSA function (OpenGL 4.5+)
		glCreateVertexArrays(1, &m_VertexArrayId);
#endif
#ifdef HELIOS_DEBUG
		glObjectLabel(GL_VERTEX_ARRAY, m_VertexArrayId, -1, name.c_str());
#endif
	}

	OpenGLVertexArray::~OpenGLVertexArray()
	{
		PROFILE_FUNCTION();

		glDeleteVertexArrays(1, &m_VertexArrayId);
	}

	void OpenGLVertexArray::Bind() const
	{
		PROFILE_FUNCTION();

		glBindVertexArray(m_VertexArrayId);
	}

	void OpenGLVertexArray::Unbind() const
	{
		PROFILE_FUNCTION();

		glBindVertexArray(0);
	}

	static GLenum TranslateBufferDataTypeToOpenGLBaseType(BufferDataType type)
	{
		switch (type)
		{
		case BufferDataType::Int:      return GL_INT;
		case BufferDataType::Int2:     return GL_INT;
		case BufferDataType::Int3:     return GL_INT;
		case BufferDataType::Int4:     return GL_INT;
		case BufferDataType::Bool:     return GL_BOOL;
		case BufferDataType::Float:    return GL_FLOAT;
		case BufferDataType::Float2:   return GL_FLOAT;
		case BufferDataType::Float3:   return GL_FLOAT;
		case BufferDataType::Float4:   return GL_FLOAT;
		case BufferDataType::Mat3:     return GL_FLOAT;
		case BufferDataType::Mat4:     return GL_FLOAT;
		case BufferDataType::UByte4:   return GL_UNSIGNED_BYTE;
		default:
			ASSERT(false, "Unknown BufferDataType!");
			return 0;
		}
	}

	void OpenGLVertexArray::AddVertexBuffer(const SharedPtr<DeviceVertexBuffer>& vertex_buffer)
	{
		PROFILE_FUNCTION();

		ASSERT(vertex_buffer && !vertex_buffer->GetLayout().GetElements().empty(), "Vertex buffer has no layout!");

		m_VertexCount += vertex_buffer->GetVertexCount();
		m_VertexBuffers.push_back(vertex_buffer);

		/* 属性指针要等知道着色器声明的 location 才能配，见 ConfigureAttributes */
		m_AttributeFingerprint = 0;
	}

	void OpenGLVertexArray::ConfigureAttributes(const ShaderReflectionData& reflection)
	{
		PROFILE_FUNCTION();

		if (m_VertexBuffers.empty())
			return;

		/* 着色器反射为空（内嵌源码等）时退回按元素顺序分配 */
		const bool named = !reflection.VertexAttributeLocations.empty();

		uint64_t fingerprint = 0xCBF29CE484222325ULL;
		const auto mix = [&fingerprint](uint64_t value)
		{
			fingerprint ^= value;
			fingerprint *= 1099511628211ULL;
		};

		for (size_t buffer_index = 0; buffer_index < m_VertexBuffers.size(); ++buffer_index)
		{
			const auto& layout = m_VertexBuffers[buffer_index]->GetLayout();
			for (const auto& element : layout.GetElements())
			{
				if (named && reflection.FindVertexAttributeLocation(element.Name) < 0)
					continue;

				mix(static_cast<uint64_t>(element.Type));
				mix(static_cast<uint64_t>(element.Offset));
				mix(static_cast<uint64_t>(buffer_index));
				mix(static_cast<uint64_t>(named ? reflection.FindVertexAttributeLocation(element.Name) : 0));
			}
		}

		/* 布局和着色器都没变：属性指针还挂着，直接跳过 */
		if (fingerprint == m_AttributeFingerprint)
			return;

		glBindVertexArray(m_VertexArrayId);

		/* 先清掉上一次的配置：同一份 VAO 可能被不同着色器用过 */
		GLint max_attributes = 0;
		glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &max_attributes);
		for (GLint index = 0; index < max_attributes; ++index)
			glDisableVertexAttribArray(static_cast<GLuint>(index));

		uint32_t sequential_location = 0;

		for (size_t buffer_index = 0; buffer_index < m_VertexBuffers.size(); ++buffer_index)
		{
			m_VertexBuffers[buffer_index]->Bind();

			const auto& layout = m_VertexBuffers[buffer_index]->GetLayout();
			for (const auto& element : layout.GetElements())
			{
				uint32_t location = sequential_location;
				if (named)
				{
					const int reflected = reflection.FindVertexAttributeLocation(element.Name);

					/* 布局里多出来的属性（着色器不读）直接忽略 */
					if (reflected < 0)
						continue;
					location = static_cast<uint32_t>(reflected);
				}

				const auto count = element.GetComponentCount();
				const auto stride = layout.GetStride();
				const auto base_type = TranslateBufferDataTypeToOpenGLBaseType(element.Type);

				switch (element.Type)
				{
				case BufferDataType::Int:
				case BufferDataType::Int2:
				case BufferDataType::Int3:
				case BufferDataType::Int4:
				case BufferDataType::Bool:
					glEnableVertexAttribArray(location);
					glVertexAttribIPointer(location, count, base_type, stride, (const void*)element.Offset);
					break;
				case BufferDataType::Float:
				case BufferDataType::Float2:
				case BufferDataType::Float3:
				case BufferDataType::Float4:
					glEnableVertexAttribArray(location);
					glVertexAttribPointer(location, count, base_type,
						element.Normalized ? GL_TRUE : GL_FALSE, stride, (const void*)element.Offset);
					break;
				case BufferDataType::UByte4:
					glEnableVertexAttribArray(location);
					glVertexAttribPointer(location, count, base_type,
						GL_TRUE,  /* UByte4 恒定归一化 [0,255] -> [0,1] */
						stride, (const void*)element.Offset);
					break;
				case BufferDataType::Mat3:
				case BufferDataType::Mat4:
					/* 矩阵按列各占一个 location，四元数实例化还要按实例步进 */
					for (uint32_t column = 0; column < count; ++column)
					{
						glEnableVertexAttribArray(location + column);
						glVertexAttribPointer(location + column, count, base_type,
							element.Normalized ? GL_TRUE : GL_FALSE, stride,
							(const void*)(element.Offset + sizeof(float) * count * column));
						glVertexAttribDivisor(location + column, 1);
					}
					break;
				default:
					ASSERT(false, "Unknown BufferDataType!");
					break;
				}

				if (!named)
				{
					const uint32_t slots = (element.Type == BufferDataType::Mat3) ? 3u
						: (element.Type == BufferDataType::Mat4) ? 4u : 1u;
					sequential_location += slots;
				}
			}
		}

		glBindVertexArray(0);
		m_AttributeFingerprint = fingerprint;
	}

	void OpenGLVertexArray::SetIndexBuffer(const SharedPtr<IndexBuffer>& index_buffer)
	{
		PROFILE_FUNCTION();

		glBindVertexArray(m_VertexArrayId);
		index_buffer->Bind();

		m_IndexBuffer = index_buffer;
	}

	uint32_t OpenGLVertexArray::GetVertexCount() const
	{
		return m_IndexBuffer ? m_IndexBuffer->GetCount() : m_VertexCount;
	}
}
