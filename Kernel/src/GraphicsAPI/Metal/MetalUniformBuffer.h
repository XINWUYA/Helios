#pragma once
#ifdef PLATFORM_MACOS

#include "Helios/VirtualDevice/DeviceUniformBuffer.h"
#include <Metal/Metal.hpp>
#include <cstdint>
#include <vector>

namespace Helios
{
    /* 每帧一段的常量环形缓冲：uniform 数据先写进这一大块，绘制时只传偏移。
     * 直接 setBytes 是把数据拷进命令缓冲，逐 draw 拷一遍白占带宽。 */
    class MetalUniformRing
    {
    public:
        ~MetalUniformRing();

        static MetalUniformRing& Instance();

        /* 要一段对齐的空间；这一帧写满时返回 false，调用方回退到 setBytes */
        bool Allocate(uint32_t size, uint32_t& out_offset);

        [[nodiscard]] MTL::Buffer* GetBuffer() const { return m_Buffer; }

    private:
        MetalUniformRing() = default;

        /* 换帧就把游标归零；缓冲在第一次用到时分配 */
        void EnsureFrame();

        MTL::Buffer* m_Buffer{ nullptr };
        uint32_t m_SliceSize{ 0 };  /* 每帧可用字节数 */
        uint32_t m_Cursor{ 0 };     /* 当前帧已用字节数 */
        uint32_t m_FrameIndex{ 0xFFFFFFFFu };
    };

    /* uniform 缓冲区的写入和绑定策略：数据写进按帧切片的环形缓冲，绑定只带偏移；
     * 环形缓冲写满或设备不可用时退回 setBytes 或独立缓冲，三条路径都由 Bind() 处理。 */
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
        /* 数据没改、又在同一个编码器里，就没有再下发的必要：视图 / 光照这类
         * 整帧不变的块每个绘制都重传一次纯属浪费。 */
        bool m_Dirty{ true };
        uint32_t m_BoundGeneration{ 0 };        /* 上次下发的编码器世代号 */
    };
}

#endif /* PLATFORM_MACOS */
