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
        constexpr uint32_t TimestampSize = sizeof(uint64_t);

        /* Apple GPU 的时间戳计数器以纳秒为单位，引擎侧统一使用微秒 */
        constexpr double NanosecondsToMicroseconds = 1e-3;
    }

    MetalQueryBuffer::MetalQueryBuffer()
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

        MTL::Device* device = MetalRuntime::Device();
        if (!device)
        {
            CORE_LOG_ERROR("MetalQueryBuffer: Metal device is not available");
            return;
        }

        /* 时间戳只能在渲染通道的阶段边界采样：Apple GPU 不支持 draw / blit / dispatch
         * 边界采样。设备不支持时停用本缓冲，解析按空表返回，UI 以占位显示。 */
        if (!device->supportsCounterSampling(MTL::CounterSamplingPointAtStageBoundary))
        {
            CORE_LOG_INFO("MetalQueryBuffer: device does not support counter sampling at stage boundary, "
                "GPU timing is disabled");
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
            CORE_LOG_WARN("MetalQueryBuffer: timestamp counter set not found, GPU timing disabled");
            return;
        }

        MTL::CounterSampleBufferDescriptor* descriptor = MTL::CounterSampleBufferDescriptor::alloc()->init();
        descriptor->setCounterSet(timestamp_counter_set);
        descriptor->setSampleCount(MAX_PASS_COUNT * SAMPLES_PER_PASS);
        /* 采样缓冲区只支持 Shared 存储模式，解析结果无需额外的 blit 同步 */
        descriptor->setStorageMode(MTL::StorageModeShared);

        NS::Error* error = nullptr;
        m_CounterSampleBuffer = device->newCounterSampleBuffer(descriptor, &error);
        descriptor->release();

        if (!m_CounterSampleBuffer)
        {
            NS::String* reason = error ? error->localizedDescription() : nullptr;
            CORE_LOG_WARN("MetalQueryBuffer: failed to create counter sample buffer ({})",
                (reason && reason->utf8String()) ? reason->utf8String() : "unknown");
        }

        if (error)
            error->release();
    }

    MetalQueryBuffer::~MetalQueryBuffer()
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

    void MetalQueryBuffer::CaptureCommandBuffer()
    {
        auto* render_api = dynamic_cast<MetalRenderAPI*>(Renderer::GetRenderAPI().get());
        MTL::CommandBuffer* command_buffer = render_api ? render_api->GetCurrentCommandBuffer() : nullptr;
        if (command_buffer == m_CommandBuffer)
            return;

        if (m_CommandBuffer)
            m_CommandBuffer->release();

        m_CommandBuffer = command_buffer;
        if (m_CommandBuffer)
            m_CommandBuffer->retain();
    }

    uint32_t MetalQueryBuffer::AttachRenderPass(void* native_render_pass_descriptor)
    {
        if (native_render_pass_descriptor == nullptr)
            return INVALID_PASS_SLOT;

        auto* descriptor = static_cast<MTL::RenderPassDescriptor*>(native_render_pass_descriptor);
        MTL::RenderPassSampleBufferAttachmentDescriptor* attachment =
            descriptor->sampleBufferAttachments()->object(0);
        if (attachment == nullptr)
            return INVALID_PASS_SLOT;

        /* 描述符跨帧复用：先清再挂 —— 不采样的通道绝不能沿用上一帧的附件（会污染旧缓冲）。
         * 实测（M3 Pro + 校验层）：附件数组最多 4 条、只有最高非空槽生效，故固定用 0 号槽。 */
        attachment->setSampleBuffer(nullptr);

        if (m_CounterSampleBuffer == nullptr)
            return INVALID_PASS_SLOT;

        if (m_PassCount >= MAX_PASS_COUNT)
        {
            /* 超过预留容量（正常帧远不会到）：该通道不采样，树里对应作用域显示占位 */
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                CORE_LOG_WARN("MetalQueryBuffer: more than {} render passes in one frame, "
                    "further passes are not sampled", MAX_PASS_COUNT);
            }
            return INVALID_PASS_SLOT;
        }

        /* 四个阶段边界采样点全挂：作用域的耗时取"stage 内 work 的起止差之和"
         * （HUD / 抓帧的 encoder GPU time 同口径）—— 只挂首尾时，紧随重通道的
         * 轻通道会把前者的流水线等待算进自己的跨度（实测能差 10 倍以上） */
        const uint32_t pass_slot = m_PassCount;
        attachment->setSampleBuffer(m_CounterSampleBuffer);
        attachment->setStartOfVertexSampleIndex(pass_slot * SAMPLES_PER_PASS + 0);
        attachment->setEndOfVertexSampleIndex(pass_slot * SAMPLES_PER_PASS + 1);
        attachment->setStartOfFragmentSampleIndex(pass_slot * SAMPLES_PER_PASS + 2);
        attachment->setEndOfFragmentSampleIndex(pass_slot * SAMPLES_PER_PASS + 3);

        m_SlotIsCompute[pass_slot] = false; /* 槽位跨帧复用：类型每帧重新落定 */
        CaptureCommandBuffer();
        ++m_PassCount;
        m_OpenPassSlot = pass_slot;
        return pass_slot;
    }

    void MetalQueryBuffer::ClearRenderPassAttachment(void* native_render_pass_descriptor)
    {
        /* 不采样的通道没有可补绑的对象：同时清掉"打开"标记 */
        m_OpenPassSlot = INVALID_PASS_SLOT;

        if (native_render_pass_descriptor == nullptr)
            return;

        auto* descriptor = static_cast<MTL::RenderPassDescriptor*>(native_render_pass_descriptor);
        MTL::RenderPassSampleBufferAttachmentDescriptor* attachment =
            descriptor->sampleBufferAttachments()->object(0);
        if (attachment != nullptr)
            attachment->setSampleBuffer(nullptr);
    }

    /* 计算通道（派发）：Apple GPU 同样只在通道边界采样，取编码器的首尾两个采样点。
     * 不挂的话纯计算的 Pass（如 GPU 蒙皮）在统计面板里永远没有数据。 */
    uint32_t MetalQueryBuffer::AttachComputePass(void* native_compute_pass_descriptor)
    {
        if (native_compute_pass_descriptor == nullptr || m_CounterSampleBuffer == nullptr)
            return INVALID_PASS_SLOT;

        if (m_PassCount >= MAX_PASS_COUNT)
            return INVALID_PASS_SLOT;

        auto* descriptor = static_cast<MTL::ComputePassDescriptor*>(native_compute_pass_descriptor);
        MTL::ComputePassSampleBufferAttachmentDescriptor* attachment =
            descriptor->sampleBufferAttachments()->object(0);
        if (attachment == nullptr)
            return INVALID_PASS_SLOT;

        const uint32_t pass_slot = m_PassCount;
        attachment->setSampleBuffer(m_CounterSampleBuffer);
        attachment->setStartOfEncoderSampleIndex(pass_slot * SAMPLES_PER_PASS + 0);
        attachment->setEndOfEncoderSampleIndex(pass_slot * SAMPLES_PER_PASS + 1);

        m_SlotIsCompute[pass_slot] = true;
        CaptureCommandBuffer();
        ++m_PassCount;
        return pass_slot;
    }

    bool MetalQueryBuffer::TryResolvePassTimestamps(std::vector<GPUPassTimestamp>& timestamps)
    {
        timestamps.clear();

        if (m_PassCount == 0)
            return true; /* 本帧没有渲染通道：空表（无数据） */

        if (m_CounterSampleBuffer == nullptr || m_CommandBuffer == nullptr)
            return true; /* 不可用（设备不支持 / 未捕获命令缓冲区）：按无数据解析 */

        /* 结果尚不可用（GPU 未执行完）时返回 false，调用方稍后重试 */
        const MTL::CommandBufferStatus status = m_CommandBuffer->status();
        if (status == MTL::CommandBufferStatusError)
        {
            CORE_LOG_WARN("MetalQueryBuffer: command buffer finished with an error, GPU timings are unavailable");
            return true;
        }

        if (status != MTL::CommandBufferStatusCompleted)
            return false;

        /* resolveCounterRange 返回的是自动释放对象，无需手动释放 */
        NS::Data* resolved = m_CounterSampleBuffer->resolveCounterRange(
            NS::Range::Make(0, m_PassCount * SAMPLES_PER_PASS));
        if (resolved == nullptr || resolved->length() < m_PassCount * SAMPLES_PER_PASS * TimestampSize)
        {
            CORE_LOG_WARN("MetalQueryBuffer: failed to resolve counter samples");
            return true;
        }

        /* 0 = 通道内没有对应阶段的工作（如无绘制的清屏通道）；
         * ~0 = 采样错误值（MTLCounterErrorValue）—— 换算时都落成 0，等于该阶段无数据。 */
        const auto* samples = static_cast<const uint64_t*>(resolved->bytes());

        /* 帧内原点取本帧最小的有效采样点，各通道的时间戳都在同一个计数器上 */
        uint64_t frame_origin = INVALID_QUERY_TIME;
        for (uint32_t slot = 0; slot < m_PassCount; ++slot)
        {
            const uint64_t* stage = samples + slot * SAMPLES_PER_PASS;
            const uint32_t sample_count = m_SlotIsCompute[slot] ? 2 : SAMPLES_PER_PASS;
            for (uint32_t index = 0; index < sample_count; ++index)
            {
                const uint64_t value = stage[index];
                if (value != 0 && value != INVALID_QUERY_TIME && value < frame_origin)
                    frame_origin = value;
            }
        }

        const auto to_frame_us = [frame_origin](uint64_t value) -> double
        {
            if (value == 0 || value == INVALID_QUERY_TIME
                || frame_origin == INVALID_QUERY_TIME || value < frame_origin)
                return 0.0;
            return static_cast<double>(value - frame_origin) * NanosecondsToMicroseconds;
        };

        /* 片元阶段在 TBDR 上是串行的，但 startOfFragment 采样点在"片元管线就绪"时就打点 —— 紧跟在
         * 重通道后面的通道会把这时间算进自己的 f（实测膨胀 4~5 倍）。修正：f 的起点不早于"本通道
         * 之前最后一个完成的片元终点"。 */
        std::vector<uint64_t> fragment_ends;
        fragment_ends.reserve(m_PassCount);
        for (uint32_t slot = 0; slot < m_PassCount; ++slot)
        {
            const uint64_t fragment_end = samples[slot * SAMPLES_PER_PASS + 3];
            if (fragment_end != 0 && fragment_end != INVALID_QUERY_TIME)
                fragment_ends.push_back(fragment_end);
        }
        std::sort(fragment_ends.begin(), fragment_ends.end());

        timestamps.resize(m_PassCount);
        for (uint32_t slot = 0; slot < m_PassCount; ++slot)
        {
            const uint64_t* stage = samples + slot * SAMPLES_PER_PASS;

            /* 计算通道：编码器首尾一对采样点即可，没有 v/f 之分 */
            if (m_SlotIsCompute[slot])
            {
                timestamps[slot].ComputeBeginUs = to_frame_us(stage[0]);
                timestamps[slot].ComputeEndUs = to_frame_us(stage[1]);
                continue;
            }

            timestamps[slot].VertexBeginUs = to_frame_us(stage[0]);
            timestamps[slot].VertexEndUs = to_frame_us(stage[1]);

            uint64_t fragment_begin = stage[2];
            const uint64_t fragment_end = stage[3];
            const bool fragment_valid = fragment_begin != 0 && fragment_begin != INVALID_QUERY_TIME
                && fragment_end != 0 && fragment_end != INVALID_QUERY_TIME && fragment_end >= fragment_begin;
            if (fragment_valid)
            {
                const auto it = std::lower_bound(fragment_ends.begin(), fragment_ends.end(), fragment_end);
                const uint64_t prev_end = (it != fragment_ends.begin()) ? *(it - 1) : 0;
                if (prev_end > fragment_begin)
                    fragment_begin = prev_end;

                /* 起点用修正后的值：排队等待不算这个通道在干活 */
                timestamps[slot].FragmentBeginUs = to_frame_us(fragment_begin);
                timestamps[slot].FragmentEndUs = to_frame_us(fragment_end);
            }
        }

        return true;
    }

    void MetalQueryBuffer::ResetFrame()
    {
        if (m_CommandBuffer)
        {
            m_CommandBuffer->release();
            m_CommandBuffer = nullptr;
        }
        m_PassCount = 0;
        m_OpenPassSlot = INVALID_PASS_SLOT;
    }

    void MetalQueryNode::Begin()
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

        /* 每帧重新绑定：本帧没有渲染通道经过时保持"无数据" */
        m_PassSlots.clear();
        m_pPassTimestamps = nullptr;
        HasValidSamples = false;
    }

    void MetalQueryNode::OnRenderPassBegin(uint32_t pass_slot)
    {
        /* 连续同通道去重（补绑与派发不会落在同一通道上，防御性） */
        if (!m_PassSlots.empty() && m_PassSlots.back() == pass_slot)
            return;

        m_PassSlots.emplace_back(pass_slot);
    }

    void MetalQueryNode::SetPassTimestamps(const std::vector<GPUPassTimestamp>* pass_timestamps)
    {
        m_pPassTimestamps = pass_timestamps;
    }

    bool MetalQueryNode::GetQueryResult()
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

        HasValidSamples = false;
        ResultTimeBegin = 0.0;
        ResultTimeEnd = 0.0;
        ResultVertexUs = 0.0;
        ResultFragmentUs = 0.0;
        ResultComputeUs = 0.0;

        if (m_pPassTimestamps == nullptr || m_PassSlots.empty())
            return true; /* 已解析：本帧无数据 */

        const std::vector<GPUPassTimestamp>& pass_timestamps = *m_pPassTimestamps;

        /* 作用域占用取绑定通道的阶段窗口并集：相邻通道的顶点与片元在 TBDR 上交叠，
         * 逐个相加会重复计同一段时间；通道首尾跨度里还夹着排队空档，也不能用。
         * 并集单调，父作用域恒不小于子作用域，树根就是帧合计。 */
        std::vector<std::pair<double, double>> spans;
        std::vector<std::pair<double, double>> vertex_spans;
        std::vector<std::pair<double, double>> fragment_spans;
        std::vector<std::pair<double, double>> compute_spans;
        spans.reserve(m_PassSlots.size() * 2);
        for (const uint32_t slot : m_PassSlots)
        {
            if (slot >= pass_timestamps.size())
                continue;

            const GPUPassTimestamp& pass = pass_timestamps[slot];
            if (pass.HasVertex())
            {
                spans.emplace_back(pass.VertexBeginUs, pass.VertexEndUs);
                vertex_spans.emplace_back(pass.VertexBeginUs, pass.VertexEndUs);
            }
            if (pass.HasFragment())
            {
                spans.emplace_back(pass.FragmentBeginUs, pass.FragmentEndUs);
                fragment_spans.emplace_back(pass.FragmentBeginUs, pass.FragmentEndUs);
            }
            if (pass.HasCompute())
            {
                spans.emplace_back(pass.ComputeBeginUs, pass.ComputeEndUs);
                compute_spans.emplace_back(pass.ComputeBeginUs, pass.ComputeEndUs);
            }
        }

        const double total_us = UnionSpanUs(std::move(spans));
        if (total_us <= 0.0)
            return true; /* 绑定通道都没有有效阶段窗口：无数据 */

        /* 下游按 end - begin 取值：begin 恒 0，end 即占用时长 */
        ResultTimeEnd = total_us;
        ResultVertexUs = UnionSpanUs(std::move(vertex_spans));
        ResultFragmentUs = UnionSpanUs(std::move(fragment_spans));
        ResultComputeUs = UnionSpanUs(std::move(compute_spans));
        HasValidSamples = true;
        return true;
    }
}

#endif /* PLATFORM_MACOS */
