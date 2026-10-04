#pragma once

#ifdef PLATFORM_MACOS

#include "Helios/VirtualDevice/DeviceVertexArray.h"
#include "MetalBuffer.h"
#include <Metal/Metal.hpp>
#include <vector>

namespace Helios
{
    class MetalVertexArray : public DeviceVertexArray
    {
    public:
        MetalVertexArray(const std::string& name);
        ~MetalVertexArray() override;

        void Bind() const override;
        void Unbind() const override;

        void AddVertexBuffer(const SharedPtr<DeviceVertexBuffer>& vertex_buffer) override;
        void SetIndexBuffer(const SharedPtr<IndexBuffer>& index_buffer) override;

        const std::vector<SharedPtr<DeviceVertexBuffer>>& GetVertexBuffers() const override { return m_VertexBuffers; }
        const SharedPtr<IndexBuffer>& GetIndexBuffer() const override { return m_IndexBuffer; }

        /* Metal特有接口 */
        void Bind(MTL::RenderCommandEncoder* encoder);
        MTL::VertexDescriptor* GetVertexDescriptor() const { return m_VertexDescriptor; }
        /* 顶点布局的结构化哈希（含 buffer stride 与各 attribute 的类型/偏移）。
         * MetalShader 用它作为 PipelineState 的缓存键，避免每帧重建管线。 */
        uint64_t GetVertexDescriptorHash() const { return m_VertexDescriptorHash; }
        uint32_t GetVertexCount() const override;

    private:
        void BuildVertexDescriptor();

        std::vector<SharedPtr<DeviceVertexBuffer>> m_VertexBuffers;
        SharedPtr<IndexBuffer> m_IndexBuffer{ nullptr };
        MTL::VertexDescriptor* m_VertexDescriptor{ nullptr };
        uint64_t m_VertexDescriptorHash{ 0 };
    };
}

#endif /* PLATFORM_MACOS */
