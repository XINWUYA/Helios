#ifdef PLATFORM_MACOS

#include "Pch.h"
#include "MetalBuffer.h"
#include "MetalCommon.h"
#include "MetalRenderAPI.h"
#include "Helios/Renderer/Renderer.h"

namespace Helios
{
    /* MetalVertexBuffer实现 */
    MetalVertexBuffer::MetalVertexBuffer(const std::string& name, uint32_t size)
        : m_Size(size)
    {
        auto device = dynamic_cast<MetalRenderAPI*>(Renderer::GetRenderAPI().get())->GetDevice();
        m_Buffer = device->newBuffer(size, MTL::ResourceOptionCPUCacheModeDefault);
        if (!m_Buffer)
        {
            CORE_LOG_ERROR("Failed to create Metal vertex buffer!");
        }
    }

    MetalVertexBuffer::MetalVertexBuffer(const std::string& name, const void* vertices, uint32_t size)
        : m_Size(size)
    {
        auto device = dynamic_cast<MetalRenderAPI*>(Renderer::GetRenderAPI().get())->GetDevice();
        m_Buffer = device->newBuffer(vertices, size, MTL::ResourceOptionCPUCacheModeDefault);
        if (!m_Buffer)
        {
            CORE_LOG_ERROR("Failed to create Metal vertex buffer!");
        }
    }

    MetalVertexBuffer::MetalVertexBuffer(const std::string& name, MTL::Buffer* buffer, uint32_t size)
        : m_Size(size)
    {
        (void)name;
        m_Buffer = buffer;
        if (m_Buffer != nullptr)
            m_Buffer->retain();
    }

    MetalVertexBuffer::~MetalVertexBuffer()
    {
        if (m_Buffer)
            m_Buffer->release();
    }

    void MetalVertexBuffer::Bind() const
    {
        /* Metal中顶点缓冲区在绘制时绑定 */
    }

    void MetalVertexBuffer::Unbind() const
    {
        /* Metal中无需解绑 */
    }

    void MetalVertexBuffer::SetData(const void* data, uint32_t size)
    {
        if (!m_Buffer || size > m_Size)
        {
            CORE_LOG_ERROR("Invalid buffer or size!");
            return;
        }

        void* buffer_data = m_Buffer->contents();
        if (buffer_data)
        {
            memcpy(buffer_data, data, size);
            /* 对于共享存储模式的缓冲区，不需要调用didModifyRange */
            /* 数据会自动同步到GPU */
            //m_Buffer->didModifyRange({ 0, size });
        }
    }

    void MetalVertexBuffer::SetLayout(const VertexBufferLayout& layout)
    {
        m_Layout = layout;
    }

    uint32_t MetalVertexBuffer::GetVertexCount() const
    {
        if (m_Layout.GetStride() == 0)
            return 0;
        return m_Size / m_Layout.GetStride();
    }

    /* MetalIndexBuffer实现 */
    /* MetalIndexBuffer实现 - 带初始数据 */
    MetalIndexBuffer::MetalIndexBuffer(const std::string& name, const void* indices, uint32_t count, IndexType type)
        : m_Count(count), m_IndexType(type)
    {
        const uint32_t index_size = (type == IndexType::UInt16) ? sizeof(uint16_t) : sizeof(uint32_t);
        const uint32_t size_in_bytes = count * index_size;

        auto device = dynamic_cast<MetalRenderAPI*>(Renderer::GetRenderAPI().get())->GetDevice();
        m_Buffer = device->newBuffer(indices, size_in_bytes, MTL::ResourceOptionCPUCacheModeDefault);
        if (!m_Buffer)
        {
            CORE_LOG_ERROR("Failed to create Metal index buffer!");
        }
    }

    /* MetalIndexBuffer实现 - 预分配空容量 */
    MetalIndexBuffer::MetalIndexBuffer(const std::string& name, uint32_t count, IndexType type)
        : m_Count(count), m_IndexType(type)
    {
        const uint32_t index_size = (type == IndexType::UInt16) ? sizeof(uint16_t) : sizeof(uint32_t);
        const uint32_t size_in_bytes = count * index_size;

        auto device = dynamic_cast<MetalRenderAPI*>(Renderer::GetRenderAPI().get())->GetDevice();
        m_Buffer = device->newBuffer(size_in_bytes, MTL::ResourceOptionCPUCacheModeDefault);
        if (!m_Buffer)
        {
            CORE_LOG_ERROR("Failed to create empty Metal index buffer!");
        }
    }

    MetalIndexBuffer::~MetalIndexBuffer()
    {
        if (m_Buffer)
            m_Buffer->release();
    }

    void MetalIndexBuffer::Bind() const
    {
        /* Metal中索引缓冲区在绘制时绑定 */
    }

    void MetalIndexBuffer::Unbind() const
    {
        /* Metal中无需解绑 */
    }

    void MetalIndexBuffer::SetData(const void* data, uint32_t size)
    {
        if (!m_Buffer)
        {
            CORE_LOG_ERROR("MetalIndexBuffer::SetData: invalid buffer!");
            return;
        }
        if (size > m_Buffer->length())
        {
            CORE_LOG_ERROR("MetalIndexBuffer::SetData: size ({}) exceeds buffer capacity ({})", size, (uint32_t)m_Buffer->length());
            return;
        }

        void* buffer_data = m_Buffer->contents();
        if (buffer_data)
        {
            memcpy(buffer_data, data, size);
            /* 共享存储模式下不需didModifyRange，数据会自动同步到GPU */
        }

        /* 根据字节数重新计算索引数量 */
        const uint32_t index_size = (m_IndexType == IndexType::UInt16) ? sizeof(uint16_t) : sizeof(uint32_t);
        m_Count = size / index_size;
    }
}

#endif /* PLATFORM_MACOS */

namespace Helios
{
    MetalStorageBuffer::MetalStorageBuffer(const std::string& name, const void* data, uint32_t size)
        : m_Size(size)
    {
        (void)name;
        auto* render_api = dynamic_cast<MetalRenderAPI*>(Renderer::GetRenderAPI().get());
        if (render_api == nullptr || render_api->GetDevice() == nullptr)
        {
            CORE_LOG_ERROR("Metal device is not ready for storage buffer!");
            return;
        }

        m_Buffer = data != nullptr
            ? render_api->GetDevice()->newBuffer(data, size, MTL::ResourceStorageModeShared)
            : render_api->GetDevice()->newBuffer(size, MTL::ResourceStorageModeShared);
        if (m_Buffer == nullptr)
            CORE_LOG_ERROR("Failed to create Metal storage buffer!");
    }

    MetalStorageBuffer::~MetalStorageBuffer()
    {
        if (m_Buffer != nullptr)
        {
            m_Buffer->release();
            m_Buffer = nullptr;
        }
    }

    void MetalStorageBuffer::SetData(const void* data, uint32_t size)
    {
        if (m_Buffer == nullptr || data == nullptr || size > m_Size)
            return;

        std::memcpy(m_Buffer->contents(), data, size);
    }

    void* MetalStorageBuffer::GetMappedData()
    {
        return m_Buffer != nullptr ? m_Buffer->contents() : nullptr;
    }
}
