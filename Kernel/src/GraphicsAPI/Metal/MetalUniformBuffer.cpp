#ifdef PLATFORM_MACOS

#include "Pch.h"
#include "MetalUniformBuffer.h"
#include "MetalCommon.h"

namespace Helios
{
    MetalUniformBuffer::MetalUniformBuffer(uint32_t size, uint32_t binding_point)
        : m_Size(size), m_BindingPoint(binding_point)
    {
        PROFILE_FUNCTION();

        if (size == 0)
        {
            CORE_LOG_ERROR("MetalUniformBuffer: zero-sized uniform buffer");
            return;
        }

        m_MetalBufferIndex = MetalBinding::ToBufferIndex(binding_point);
        m_Shadow.assign(size, 0);

        /* 小数据随命令缓冲区提交，天然按绘制隔离，无需分配设备内存 */
        m_UseInlineBytes = size <= MetalBinding::MaxInlineBytes;
        if (m_UseInlineBytes)
            return;

        MTL::Device* device = MetalRuntime::Device();
        if (!device)
        {
            /* 设备不可用时退化为内联，避免空指针崩溃 */
            CORE_LOG_WARN("MetalUniformBuffer: Metal device unavailable, using inline bytes for {} bytes", size);
            m_UseInlineBytes = true;
            return;
        }

        /* 按帧切片。切片按 MetalStorage::UniformSliceAlignment 对齐：
         * constant 缓冲区在部分 GPU 上要求较高的 offset 对齐。 */
        const uint32_t alignment = MetalStorage::UniformSliceAlignment;
        m_SliceSize = (size + alignment - 1u) / alignment * alignment;

        const NS::UInteger total_size =
            static_cast<NS::UInteger>(m_SliceSize) * MetalConfig::MaxFramesInFlight;
        m_Buffer = device->newBuffer(total_size, MetalStorage::UniformBufferOptions);
        if (!m_Buffer)
        {
            CORE_LOG_ERROR("MetalUniformBuffer: failed to allocate {} bytes for binding {}",
                total_size, binding_point);
        }
    }

    MetalUniformBuffer::~MetalUniformBuffer()
    {
        PROFILE_FUNCTION();

        if (m_Buffer)
        {
            m_Buffer->release();
            m_Buffer = nullptr;
        }
    }

    void MetalUniformBuffer::SetData(const void* data, uint32_t size, uint32_t offset)
    {
        PROFILE_FUNCTION();

        if (!data || size == 0)
            return;

        if (offset + size > m_Size)
        {
            CORE_LOG_ERROR("MetalUniformBuffer: write out of range ({} + {} > {})",
                offset, size, m_Size);
            return;
        }

        /* 只更新 CPU 影子数据：设备端数据在 Bind 时按绘制/按帧落位，
         * 避免同帧内多次写入互相覆盖。 */
        memcpy(m_Shadow.data() + offset, data, size);
    }

    void MetalUniformBuffer::Bind()
    {
        PROFILE_FUNCTION();

        MTL::RenderCommandEncoder* encoder = MetalRuntime::Encoder();
        if (!encoder)
            return;

        if (m_UseInlineBytes)
        {
            /* setBytes 把数据拷贝进当前命令缓冲区，每次绘制的数据互不干扰，
             * 可安全覆写 CPU 端影子数据。 */
            encoder->setVertexBytes(m_Shadow.data(), m_Size, m_MetalBufferIndex);
            encoder->setFragmentBytes(m_Shadow.data(), m_Size, m_MetalBufferIndex);
            return;
        }

        if (!m_Buffer)
            return;

        /* 绑定当前帧对应的切片；帧数上限由 drawable 数量保证，
         * 该切片对应的命令缓冲区已执行完毕。 */
        const uint32_t offset = MetalRuntime::FrameIndex() * m_SliceSize;
        memcpy(static_cast<uint8_t*>(m_Buffer->contents()) + offset, m_Shadow.data(), m_Size);

        encoder->setVertexBuffer(m_Buffer, offset, m_MetalBufferIndex);
        encoder->setFragmentBuffer(m_Buffer, offset, m_MetalBufferIndex);
    }
}

#endif /* PLATFORM_MACOS */
