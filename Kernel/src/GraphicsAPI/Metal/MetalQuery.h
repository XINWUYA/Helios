#pragma once

#ifdef PLATFORM_MACOS

#include "Helios/VirtualDevice/DeviceQueryNode.h"
#include <Metal/Metal.hpp>

namespace Helios
{
    /* 基于 MTL::CounterSampleBuffer 的 GPU 时间戳查询。
     * 采样只能在渲染编码器内部（draw boundary）进行，结果需待承载命令缓冲区完成
     * 后才能解析，因此同时持有采样缓冲区与最近一次采样的命令缓冲区引用。 */
    class MetalQueryNode final : public DeviceQueryNode
    {
    public:
        MetalQueryNode();
        ~MetalQueryNode() override;

        void Begin() override;
        void End() override;
        bool GetQueryResult() override;

    private:
        /* 在当前渲染通道上记录一个时间戳，成功返回 true */
        bool Sample(uint32_t sampleIndex);

        MTL::CounterSampleBuffer* m_CounterSampleBuffer{ nullptr };
        MTL::CommandBuffer* m_CommandBuffer{ nullptr };
        bool m_HasBegin{ false };
        bool m_HasEnd{ false };
    };
}

#endif /* PLATFORM_MACOS */
