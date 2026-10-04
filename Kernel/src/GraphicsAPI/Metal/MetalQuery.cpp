#ifdef PLATFORM_MACOS

#include "Pch.h"
#include "MetalQuery.h"
#include "MetalCommon.h"
#include "MetalRenderAPI.h"
#include "Helios/Renderer/Renderer.h"

namespace Helios
{
    namespace
    {
        /* 一对时间戳：Begin / End */
        constexpr NS::UInteger TimestampSampleCount = 2;
        constexpr uint32_t BeginSampleIndex = 0;
        constexpr uint32_t EndSampleIndex = 1;
        constexpr uint32_t TimestampSize = sizeof(uint64_t);

        /* Apple GPU 的时间戳计数器以纳秒为单位，引擎侧统一使用微秒 */
        constexpr double NanosecondsToMicroseconds = 1e-3;
    }

    MetalQueryNode::MetalQueryNode()
    {
        PROFILE_FUNCTION();

        MTL::Device* device = MetalRuntime::Device();
        if (!device)
        {
            CORE_LOG_ERROR("MetalQueryNode: Metal device is not available");
            return;
        }

        /* sampleCountersInBuffer 要求设备支持 draw boundary 采样；
         * 不支持时停用本节点，GetQueryResult 恒返回 false，调用方沿用上一帧数值。 */
        if (!device->supportsCounterSampling(MTL::CounterSamplingPointAtDrawBoundary))
        {
            /* 每帧都会创建大量查询节点，此处只提示一次 */
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                CORE_LOG_INFO("MetalQueryNode: device does not support counter sampling at draw boundary, "
                    "GPU timing is disabled");
            }
            return;
        }

        /* 时间戳计数器集合按名称查找：不同 GPU 家族的计数器集合内容不同，不能
         * 假定下标，名称也不保证大小写一致（系统常量为 "timestamp"）。 */
        MTL::CounterSet* timestamp_counter_set = nullptr;
        NS::Array* counter_sets = device->counterSets();
        for (NS::UInteger i = 0; counter_sets && i < counter_sets->count(); ++i)
        {
            auto* counter_set = static_cast<MTL::CounterSet*>(counter_sets->object(i));
            NS::String* name = counter_set ? counter_set->name() : nullptr;
            const char* name_utf8 = name ? name->utf8String() : nullptr;
            if (name_utf8 && strcasecmp(name_utf8, "timestamp") == 0)
            {
                timestamp_counter_set = counter_set;
                break;
            }
        }

        if (!timestamp_counter_set)
        {
            CORE_LOG_WARN("MetalQueryNode '{}': timestamp counter set not found, GPU timing disabled", Label);
            return;
        }

        MTL::CounterSampleBufferDescriptor* descriptor = MTL::CounterSampleBufferDescriptor::alloc()->init();
        descriptor->setCounterSet(timestamp_counter_set);
        descriptor->setSampleCount(TimestampSampleCount);
        /* 采样缓冲区只支持 Shared 存储模式，解析结果无需额外的 blit 同步 */
        descriptor->setStorageMode(MTL::StorageModeShared);

        NS::Error* error = nullptr;
        m_CounterSampleBuffer = device->newCounterSampleBuffer(descriptor, &error);
        descriptor->release();

        if (!m_CounterSampleBuffer)
        {
            NS::String* reason = error ? error->localizedDescription() : nullptr;
            CORE_LOG_WARN("MetalQueryNode '{}': failed to create counter sample buffer ({})",
                Label, (reason && reason->utf8String()) ? reason->utf8String() : "unknown");
        }

        if (error)
            error->release();
    }

    MetalQueryNode::~MetalQueryNode()
    {
        if (m_CommandBuffer)
        {
            m_CommandBuffer->release();
            m_CommandBuffer = nullptr;
        }

        if (m_CounterSampleBuffer)
        {
            m_CounterSampleBuffer->release();
            m_CounterSampleBuffer = nullptr;
        }
    }

    bool MetalQueryNode::Sample(uint32_t sampleIndex)
    {
        if (!m_CounterSampleBuffer)
            return false;

        /* 时间戳只能在渲染通道内采样；无活动编码器时放弃本次采样，
         * 由 GetQueryResult 返回失败。 */
        MTL::RenderCommandEncoder* encoder = MetalRuntime::Encoder();
        if (!encoder)
        {
            CORE_LOG_WARN("MetalQueryNode '{}': no active render encoder, GPU timing skipped", Label);
            return false;
        }

        encoder->sampleCountersInBuffer(m_CounterSampleBuffer, sampleIndex, /*barrier=*/true);
        return true;
    }

    void MetalQueryNode::Begin()
    {
        PROFILE_FUNCTION();

        /* 每帧重新采样：释放上一帧的命令缓冲区引用并清空状态，
         * 保证 GetQueryResult 不会读到跨帧的陈旧结果。 */
        if (m_CommandBuffer)
        {
            m_CommandBuffer->release();
            m_CommandBuffer = nullptr;
        }
        m_HasBegin = false;
        m_HasEnd = false;

        m_HasBegin = Sample(BeginSampleIndex);
    }

    void MetalQueryNode::End()
    {
        PROFILE_FUNCTION();

        if (!m_HasBegin)
            return;

        m_HasEnd = Sample(EndSampleIndex);
        if (!m_HasEnd)
            return;

        /* 记下承载采样的命令缓冲区并 retain：只有它执行完成后采样数据才有效，
         * 且该命令缓冲区可能在引擎侧被提前释放。 */
        auto* render_api = dynamic_cast<MetalRenderAPI*>(Renderer::GetRenderAPI().get());
        MTL::CommandBuffer* command_buffer = render_api ? render_api->GetCurrentCommandBuffer() : nullptr;
        if (command_buffer)
        {
            command_buffer->retain();
            m_CommandBuffer = command_buffer;
        }
    }

    bool MetalQueryNode::GetQueryResult()
    {
        PROFILE_FUNCTION();

        if (!m_CounterSampleBuffer || !m_HasBegin || !m_HasEnd)
            return false;

        /* 结果尚不可用（GPU 未执行完）时返回 false，调用方
         * （RenderQueryContext::PrepareQueryResult）会沿用上一帧的数值。 */
        if (m_CommandBuffer)
        {
            const MTL::CommandBufferStatus status = m_CommandBuffer->status();
            if (status == MTL::CommandBufferStatusError)
            {
                CORE_LOG_WARN("MetalQueryNode '{}': command buffer finished with an error", Label);
                m_HasBegin = false;
                m_HasEnd = false;
                return false;
            }

            if (status != MTL::CommandBufferStatusCompleted)
                return false;
        }

        /* resolveCounterRange 返回的是自动释放对象，无需手动释放 */
        NS::Data* resolved = m_CounterSampleBuffer->resolveCounterRange(
            NS::Range::Make(BeginSampleIndex, TimestampSampleCount));
        if (!resolved || resolved->length() < TimestampSampleCount * TimestampSize)
            return false;

        const auto* samples = static_cast<const uint64_t*>(resolved->bytes());
        QueryTimeBegin = samples[0];
        QueryTimeEnd = samples[1];

        ResultTimeBegin = static_cast<double>(QueryTimeBegin) * NanosecondsToMicroseconds;
        ResultTimeEnd = static_cast<double>(QueryTimeEnd) * NanosecondsToMicroseconds;

        return true;
    }
}

#endif /* PLATFORM_MACOS */
