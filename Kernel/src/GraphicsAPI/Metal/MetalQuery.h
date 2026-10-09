#pragma once

#ifdef PLATFORM_MACOS

#include "Helios/VirtualDevice/DeviceQueryNode.h"
#include "Helios/VirtualDevice/DeviceQueryBuffer.h"
#include <Metal/Metal.hpp>

namespace Helios
{
    /* 基于 MTL::CounterSampleBuffer 的通道时间戳缓冲。Apple GPU 只有渲染通道的阶段边界能采样，
     * 每个通道占四个采样下标（顶点起止 + 片元起止），共用一张缓冲；命令完成后一次性解析成时长表。 */
    class MetalQueryBuffer final : public DeviceQueryBuffer
    {
    public:
        MetalQueryBuffer();
        ~MetalQueryBuffer() override;

        uint32_t AttachRenderPass(void* native_render_pass_descriptor) override;
        void ClearRenderPassAttachment(void* native_render_pass_descriptor) override;
        uint32_t GetOpenRenderPassSlot() const override { return m_OpenPassSlot; }
        void OnRenderPassEnd() override { m_OpenPassSlot = INVALID_PASS_SLOT; }
        bool TryResolvePassTimestamps(std::vector<GPUPassTimestamp>& timestamps) override;
        void ResetFrame() override;

    private:
        /* 记下承载采样的命令缓冲区（retain）：结果必须等它执行完成才有效，
         * 且该命令缓冲区可能在引擎侧被提前释放。 */
        void CaptureCommandBuffer();

        MTL::CounterSampleBuffer* m_CounterSampleBuffer{ nullptr };
        MTL::CommandBuffer* m_CommandBuffer{ nullptr };
        uint32_t m_PassCount{ 0 };
        /* 当前仍打开的采样通道（编码器已创建、尚未关闭）；无 = INVALID */
        uint32_t m_OpenPassSlot{ INVALID_PASS_SLOT };
    };

    /* 通道绑定模型的查询节点：Begin/End 只清/记状态，时间戳在解析时由
     * 本帧绑定的全部通道累加合成（stage 时长和，见 DeviceQueryBuffer）。 */
    class MetalQueryNode final : public DeviceQueryNode
    {
    public:
        void Begin() override;
        bool GetQueryResult() override;

        void OnRenderPassBegin(uint32_t pass_slot) override;
        void SetPassTimestamps(const std::vector<GPUPassTimestamp>* pass_timestamps) override;

        bool SupportsStageSplit() const override { return true; }

    private:
        /* 本帧绑定的全部通道序号（逐帧复用、Begin 时清空） */
        std::vector<uint32_t> m_PassSlots{};
        const std::vector<GPUPassTimestamp>* m_pPassTimestamps{ nullptr };
    };
}

#endif /* PLATFORM_MACOS */
