#pragma once
#ifdef PLATFORM_MACOS

#include "Helios/VirtualDevice/DeviceUniformBuffer.h"
#include <Metal/Metal.hpp>
#include <cstdint>
#include <vector>

namespace Helios
{
    /* uniform 缓冲区的写入和绑定策略：不超过 MaxInlineBytes 的数据走 setVertexBytes /
     * setFragmentBytes、随命令缓冲区拷贝、按绘制隔离；超过上限就用按帧切片的 MTLBuffer。
     * 两条路径都由 Bind() 统一处理。 */
    class MetalUniformBuffer : public DeviceUniformBuffer
    {
    public:
        MetalUniformBuffer(uint32_t size, uint32_t binding_point);
        ~MetalUniformBuffer() override;

        void SetData(const void* data, uint32_t size, uint32_t offset = 0) override;

        /* 绑定到当前 Metal RenderCommandEncoder 的对应槽位（顶点与片元阶段同时可用） */
        void Bind() override;

        /* 后备缓冲区；仅当数据超过内联上限时才存在。
         * 常规绑定请使用 Bind()，它同时覆盖内联与缓冲区两条路径。 */
        MTL::Buffer* GetMetalBuffer() const { return m_Buffer; }

        uint32_t GetBindingPoint() const { return m_BindingPoint; }

        /* MSL 中对应的 [[buffer(N)]] 索引 */
        [[nodiscard]] uint32_t GetMetalBufferIndex() const { return m_MetalBufferIndex; }

    private:
        std::vector<uint8_t> m_Shadow{};        /* CPU 端影子数据（权威副本） */
        MTL::Buffer* m_Buffer{ nullptr };       /* 超大数据的按帧切片后备缓冲区 */
        uint32_t m_Size{ 0 };                   /* 有效数据大小 */
        uint32_t m_BindingPoint{ 0 };           /* GLSL 中的 binding */
        uint32_t m_MetalBufferIndex{ 0 };       /* 映射后的 MSL buffer 索引 */
        uint32_t m_SliceSize{ 0 };              /* 单帧切片大小（对齐后） */
        bool m_UseInlineBytes{ true };          /* 是否走 setBytes 快速路径 */
    };
}

#endif /* PLATFORM_MACOS */
