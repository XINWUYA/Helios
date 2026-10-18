#pragma once
#include <string>

namespace Helios
{
	/* 索引缓冲区支持的数据类型 */
	enum class IndexType : uint8_t
	{
		UInt16 = 0,  // 16-bit indices (unsigned short)
		UInt32       // 32-bit indices (unsigned int)
	};

	enum class BufferDataType : uint8_t
	{
		None = 0,
		Int,
		Int2,
		Int3,
		Int4,
		Bool,
		Float,
		Float2,
		Float3,
		Float4,
		Mat3,
		Mat4,
		UByte4    // 4 bytes, normalized unsigned byte [0, 255] -> [0.0, 1.0]
	};

	struct BufferElement
	{
		std::string Name;
		BufferDataType Type;
		uint32_t Size;
		size_t Offset;
		bool Normalized;

		BufferElement() = default;
		BufferElement(const std::string& name, BufferDataType type, bool normalized = false);

		uint32_t GetComponentCount() const;
	};

	class VertexBufferLayout
	{
	public:
		VertexBufferLayout() = default;
		VertexBufferLayout(std::initializer_list<BufferElement> elements);

		uint32_t GetStride() const { return m_Stride; }
		const std::vector<BufferElement>& GetElements() const { return m_Elements; }

		/* 添加Element */
		void EmplaceElement(const BufferElement& element);

		std::vector<BufferElement>::iterator begin() { return m_Elements.begin(); }
		std::vector<BufferElement>::iterator end() { return m_Elements.end(); }
		std::vector<BufferElement>::const_iterator begin() const { return m_Elements.begin(); }
		std::vector<BufferElement>::const_iterator end() const { return m_Elements.end(); }

	private:
		void CalculateOffsetsAndStride();

		std::vector<BufferElement> m_Elements{};
		uint32_t m_Stride{ 0 };
	};

	class DeviceStorageBuffer;

	// Vertex Buffer
	class DeviceVertexBuffer
	{
	public:
		DeviceVertexBuffer() = default;
		virtual ~DeviceVertexBuffer() = default;

		virtual void Bind() const = 0;
		virtual void Unbind() const = 0;

		/* 设置顶点数据 */
		virtual void SetData(const void* data, uint32_t size) = 0;
		[[nodiscard]] virtual uint32_t GetDataSize() const = 0;

		/* 设置顶点布局 */
		virtual void SetLayout(const VertexBufferLayout& layout) = 0;
		[[nodiscard]] virtual const VertexBufferLayout& GetLayout() const = 0;

		/* 获取顶点数量 */
		[[nodiscard]] virtual uint32_t GetVertexCount() const = 0;

		static SharedPtr<DeviceVertexBuffer> Create(const std::string& name, uint32_t size);
		/* 和一块读写缓冲共用同一份显存（计算着色器写它、绘制读它）。
		 * 后端不支持时返回 nullptr，调用方退回 CPU 蒙皮。 */
		static SharedPtr<DeviceVertexBuffer> CreateShared(const std::string& name,
			const SharedPtr<DeviceStorageBuffer>& storage, uint32_t size);
		static SharedPtr<DeviceVertexBuffer> Create(const std::string& name, const void* vertices, uint32_t size);
	};

	// Index Buffer
	/* 通用读写缓冲：计算着色器的输入 / 输出（Metal 上就是 storage buffer）。
	 * 引擎这块内存是 CPU 可见的（shared），读回结果也走它。 */
	class DeviceStorageBuffer
	{
	public:
		DeviceStorageBuffer() = default;
		virtual ~DeviceStorageBuffer() = default;

		/* 整块覆盖数据（size 不能超过创建时的容量） */
		virtual void SetData(const void* data, uint32_t size) = 0;
		/* CPU 侧直接读写这块内存（Metal shared buffer 的 contents） */
		[[nodiscard]] virtual void* GetMappedData() = 0;
		[[nodiscard]] virtual uint32_t GetDataSize() const = 0;

		static SharedPtr<DeviceStorageBuffer> Create(const std::string& name, const void* data, uint32_t size);
		static SharedPtr<DeviceStorageBuffer> Create(const std::string& name, uint32_t size);
	};

	class IndexBuffer
	{
	public:
		IndexBuffer() = default;
		virtual ~IndexBuffer() = default;

		virtual void Bind() const = 0;
		virtual void Unbind() const = 0;

		virtual uint32_t GetCount() const = 0;
		virtual IndexType GetIndexType() const = 0;

		/* 更新索引缓冲区数据。size单位为字节，且必须小于等于缓冲区创建时的总容量。
		 * 仅更新数据内容，不改变索引数量/类型，也不会重建底层GPU缓冲区。 */
		virtual void SetData(const void* data, uint32_t size) = 0;

		/* 创建带初始数据的索引缓冲区（STATIC_DRAW，用于静态数据） */
		static SharedPtr<IndexBuffer> Create(const std::string& name, const void* indices, uint32_t count, IndexType type = IndexType::UInt32);
		/* 创建空容量的索引缓冲区（DYNAMIC_DRAW，预分配后由 SetData 动态更新） */
		static SharedPtr<IndexBuffer> Create(const std::string& name, uint32_t count, IndexType type = IndexType::UInt32);
	};
}
