#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Helios/VirtualDevice/DeviceBuffer.h"

namespace Helios
{
	class OpenGLVertexBuffer : public DeviceVertexBuffer
	{
	public:
		OpenGLVertexBuffer(const std::string& name, uint32_t size);
		OpenGLVertexBuffer(const std::string& name, const void* vertices, uint32_t size);
		/* 复用已有的 GL 缓冲（计算着色器写它、绘制读它，省一次拷贝）。
		 * 缓冲名归传入方所有，这里只借用、不删除。 */
		OpenGLVertexBuffer(const std::string& name, uint32_t buffer_id, uint32_t size);
		~OpenGLVertexBuffer() override;

		/* 绑定 */
		void Bind() const override;
		void Unbind() const override;

		/* 设置顶点数据 */
		void SetData(const void* data, uint32_t size) override;
		[[nodiscard]] uint32_t GetDataSize() const override { return m_DataSize; }

		/* 设置顶点布局 */
		void SetLayout(const VertexBufferLayout& layout) override { m_Layout = layout; }
		[[nodiscard]] const VertexBufferLayout& GetLayout() const override { return m_Layout; }

		/* 获取顶点数量 */
        [[nodiscard]] uint32_t GetVertexCount() const override;

	private:
		/* GPU上对应的BufferId */
		uint32_t m_VertexBufferId{ 0 };
		/* 顶点数据的大小 */
		uint32_t m_DataSize{ 0 };
		/* 顶点布局 */
		VertexBufferLayout m_Layout{};
		/* 缓冲名的所有权：复用外部缓冲时为 false，析构不删 */
		bool m_OwnsBuffer{ true };
	};


	class OpenGLIndexBuffer : public IndexBuffer
	{
	public:
		/* 创建带初始数据的索引缓冲区（STATIC_DRAW） */
		OpenGLIndexBuffer(const std::string& name, const void* indices, uint32_t count, IndexType type);
		/* 创建空容量的索引缓冲区（预分配，DYNAMIC_DRAW） */
		OpenGLIndexBuffer(const std::string& name, uint32_t count, IndexType type);
		~OpenGLIndexBuffer() override;

		/* 绑定 */
		void Bind() const override;
		void Unbind() const override;

		uint32_t GetCount() const override { return m_Count; }
		IndexType GetIndexType() const override { return m_IndexType; }

		/* 更新索引缓冲区数据 */
		void SetData(const void* data, uint32_t size) override;

	private:
		uint32_t m_IndexBufferId{ 0 };
		uint32_t m_Count{ 0 };
		IndexType m_IndexType{ IndexType::UInt32 };
	};

#ifndef __APPLE__
	/* 计算着色器用的读写缓冲（SSBO）。macOS 只有 GL 4.1，没有这个能力，不编进来。 */
	class OpenGLStorageBuffer : public DeviceStorageBuffer
	{
	public:
		OpenGLStorageBuffer(const std::string& name, const void* data, uint32_t size);
		~OpenGLStorageBuffer() override;

		void SetData(const void* data, uint32_t size) override;
		/* 从 GPU 回读当前内容（计算写完之后读结果用），返回内部镜像 */
		[[nodiscard]] void* GetMappedData() override;
		[[nodiscard]] uint32_t GetDataSize() const override { return m_Size; }

		[[nodiscard]] uint32_t GetBufferID() const { return m_BufferID; }

	private:
		uint32_t m_BufferID{ 0 };
		uint32_t m_Size{ 0 };
		/* 回读镜像（GetMappedData 的返回区） */
		std::vector<uint8_t> m_Shadow{};
	};
#endif
}

