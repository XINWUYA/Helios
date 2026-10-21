#ifdef PLATFORM_MACOS

#include "Pch.h"
#include "MetalVertexArray.h"
#include "MetalCommon.h"
#include "MetalRenderAPI.h"
#include "Helios/Renderer/Renderer.h"

namespace Helios
{
    MetalVertexArray::MetalVertexArray(const std::string& name)
    {
        BuildVertexLayoutHash();
    }

    MetalVertexArray::~MetalVertexArray() = default;

    void MetalVertexArray::Bind() const
    {
        /* Metal中顶点数组在绘制时绑定 */
    }

    void MetalVertexArray::Unbind() const
    {
        /* Metal中无需解绑 */
    }

    void MetalVertexArray::AddVertexBuffer(const SharedPtr<DeviceVertexBuffer>& vertex_buffer)
    {
        m_VertexBuffers.push_back(vertex_buffer);
        BuildVertexLayoutHash();
    }

    void MetalVertexArray::SetIndexBuffer(const SharedPtr<IndexBuffer>& index_buffer)
    {
        m_IndexBuffer = index_buffer;
    }

    void MetalVertexArray::Bind(MTL::RenderCommandEncoder* encoder)
    {
        if (!encoder)
            return;

        /* 绑定所有顶点缓冲区。槽位从 MetalBinding::VertexBufferBase 起算，
         * 与 VertexDescriptor 及 uniform buffer 的槽位约定保持一致。 */
        for (size_t i = 0; i < m_VertexBuffers.size(); ++i)
        {
            auto metal_buffer = std::dynamic_pointer_cast<MetalVertexBuffer>(m_VertexBuffers[i]);
            if (metal_buffer && metal_buffer->GetMetalBuffer())
            {
                encoder->setVertexBuffer(metal_buffer->GetMetalBuffer(), 0,
                    static_cast<NS::UInteger>(MetalBinding::VertexBufferBase + i));
            }
        }
    }

    uint32_t MetalVertexArray::GetVertexCount() const
    {
        if (m_VertexBuffers.empty())
            return 0;

        auto first_buffer = std::dynamic_pointer_cast<MetalVertexBuffer>(m_VertexBuffers[0]);
        return first_buffer ? first_buffer->GetVertexCount() : 0;
    }

    void MetalVertexArray::BuildVertexLayoutHash()
    {
        /* 顶点描述符由 MetalShader 按着色器声明的属性位置生成，这里只算布局哈希：
         * 布局不同就是不同管线，着色器那边的描述符缓存也按它分桶。 */
        uint64_t hash = 0xCBF29CE484222325ULL;
        if (m_VertexBuffers.empty())
        {
            m_VertexDescriptorHash = 0;
            return;
        }

        for (size_t buffer_index = 0; buffer_index < m_VertexBuffers.size(); ++buffer_index)
        {
            auto metal_buffer = std::dynamic_pointer_cast<MetalVertexBuffer>(m_VertexBuffers[buffer_index]);
            if (!metal_buffer)
                continue;

            const auto& layout = metal_buffer->GetLayout();
            const auto& elements = layout.GetElements();

            MetalHashCombine(hash, static_cast<uint64_t>(buffer_index));
            MetalHashCombine(hash, layout.GetStride());
            MetalHashCombine(hash, static_cast<uint64_t>(elements.size()));

            for (const auto& element : elements)
            {
                MetalHashCombine(hash, static_cast<uint64_t>(element.Type));
                MetalHashCombine(hash, static_cast<uint64_t>(element.Offset));
                MetalHashCombine(hash, static_cast<uint64_t>(element.Normalized ? 1u : 0u));
                MetalHashCombine(hash, static_cast<uint64_t>(element.GetComponentCount()));
            }
        }

        m_VertexDescriptorHash = hash;
    }
}

#endif /* PLATFORM_MACOS */
