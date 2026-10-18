#pragma once

#ifdef PLATFORM_MACOS

#include "Helios/VirtualDevice/DeviceBuffer.h"
#include <Metal/Metal.hpp>

namespace Helios
{
    class MetalVertexBuffer : public DeviceVertexBuffer
    {
    public:
        MetalVertexBuffer(const std::string& name, uint32_t size);
        MetalVertexBuffer(const std::string& name, const void* vertices, uint32_t size);
        /* 复用已有 MTL 缓冲（计算着色器写它、绘制读它，省一次拷贝） */
        MetalVertexBuffer(const std::string& name, MTL::Buffer* buffer, uint32_t size);
        ~MetalVertexBuffer() override;

        void Bind() const override;
        void Unbind() const override;

        void SetData(const void* data, uint32_t size) override;
        uint32_t GetDataSize() const override { return m_Size; }

        void SetLayout(const VertexBufferLayout& layout) override;
        const VertexBufferLayout& GetLayout() const override { return m_Layout; }

        uint32_t GetVertexCount() const override;

        MTL::Buffer* GetMetalBuffer() const { return m_Buffer; }

    private:
        MTL::Buffer* m_Buffer{ nullptr };
        uint32_t m_Size{ 0 };
        VertexBufferLayout m_Layout;
    };

    /* 计算着色器用的读写缓冲（storage buffer）：CPU 可见，读回结果也走它 */
    class MetalStorageBuffer : public DeviceStorageBuffer
    {
    public:
        MetalStorageBuffer(const std::string& name, const void* data, uint32_t size);
        ~MetalStorageBuffer() override;

        void SetData(const void* data, uint32_t size) override;
        [[nodiscard]] void* GetMappedData() override;
        [[nodiscard]] uint32_t GetDataSize() const override { return m_Size; }

        MTL::Buffer* GetMetalBuffer() const { return m_Buffer; }

    private:
        MTL::Buffer* m_Buffer{ nullptr };
        uint32_t m_Size{ 0 };
    };

    class MetalIndexBuffer : public IndexBuffer
    {
    public:
        /* 创建带初始数据的索引缓冲区 */
        MetalIndexBuffer(const std::string& name, const void* indices, uint32_t count, IndexType type);
        /* 创建空容量的索引缓冲区（预分配） */
        MetalIndexBuffer(const std::string& name, uint32_t count, IndexType type);
        ~MetalIndexBuffer() override;

        void Bind() const override;
        void Unbind() const override;

        uint32_t GetCount() const override { return m_Count; }
        IndexType GetIndexType() const override { return m_IndexType; }

        /* 更新索引缓冲区数据 */
        void SetData(const void* data, uint32_t size) override;

        MTL::Buffer* GetMetalBuffer() const { return m_Buffer; }

    private:
        MTL::Buffer* m_Buffer{ nullptr };
        uint32_t m_Count{ 0 };
        IndexType m_IndexType{ IndexType::UInt32 };
    };
}

#endif /* PLATFORM_MACOS */
