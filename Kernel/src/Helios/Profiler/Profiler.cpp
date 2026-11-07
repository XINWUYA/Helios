#include "Pch.h"
#include "Profiler.h"
#include "CPUTimeScopeTree.h"
#include "GPUTimeScopeTree.h"
#include "RenderStatistics.h"
#include "Internal/PerfettoWriter.h"
#include <Helios/Renderer/Renderer.h>

namespace Helios
{
	/* Perfetto 出口的转发：宏与面板只认这三个函数，写盘实现留在 Internal */
	namespace ProfilerTrace
	{
		bool IsSessionActive()
		{
			return PerfettoWriter::Instance().IsSessionActive();
		}

		void BeginSession(const char* name, const char* file)
		{
			PerfettoWriter::Instance().BeginSession(
				name != nullptr ? name : "",
				file != nullptr ? file : "ProfileResult-TimeCost.json");
		}

		void EndSession()
		{
			PerfettoWriter::Instance().EndSession();
		}
	}

	// CPU 作用域
	CPUTimeScope::CPUTimeScope(const char* label, uint64_t id, ProfileChannel channel,
		const char* file, uint32_t line)
	{
		CPUTimeScopeTree& profiler = CPUTimeScopeTree::Instance();
		if (!profiler.IsEnabled())
			return;

		m_NodeIndex = profiler.BeginScope(label, id, channel, file, line);
	}

	/* 直接构造的调用方（标签是运行时字符串）：就地算一次哈希，量级等同原来的一次 strcmp */
	CPUTimeScope::CPUTimeScope(const char* label, const char* file, uint32_t line)
		: CPUTimeScope(label, ProfileHash::Hash(label), ProfileChannel::General, file, line)
	{
	}

	CPUTimeScope::~CPUTimeScope()
	{
		if (m_NodeIndex == INVALID_SCOPE_INDEX)
			return;
		CPUTimeScopeTree::Instance().EndScope(m_NodeIndex);
	}

	/* 面板画自己时临时清掉 Editor 位：控制台的绘制开销不混进统计树 */
	ScopedChannelMask::ScopedChannelMask(ProfileChannel mask)
		: m_RestoreMask(CPUTimeScopeTree::Instance().GetChannelMask())
	{
		CPUTimeScopeTree::Instance().SetChannelMask(mask);
	}

	ScopedChannelMask::ScopedChannelMask(ProfileChannel mask, ProfileChannel restore)
		: m_RestoreMask(restore)
	{
		CPUTimeScopeTree::Instance().SetChannelMask(mask);
	}

	ScopedChannelMask::~ScopedChannelMask()
	{
		CPUTimeScopeTree::Instance().SetChannelMask(m_RestoreMask);
	}

	// GPU 作用域
	/* 调试分组同时驱动 GPU 计时作用域：组名是抓帧分组与统计面板共用的唯一标签源。
	 * 作用域无条件配对，不依赖编码器是否已创建 —— 调用点常先 push 组、再 Bind 开通道。 */
	GPUTimeScope::GPUTimeScope(const char* label) : m_Active(Renderer::GetRenderAPI() != nullptr)
	{
		if (m_Active)
			Renderer::GetRenderAPI()->PushDebugGroup(label);
	}

	GPUTimeScope::GPUTimeScope(const std::string& label) : GPUTimeScope(label.c_str())
	{
	}

	GPUTimeScope::~GPUTimeScope()
	{
		if (m_Active)
			Renderer::GetRenderAPI()->PopDebugGroup();
	}

	// 门面
	Profiler& Profiler::Instance()
	{
		static Profiler instance;
		return instance;
	}

	void Profiler::BeginFrame()
	{
		m_FrameBegun = true;
		m_FrameStart = std::chrono::steady_clock::now();
	}

	void Profiler::BeginCPUTiming()
	{
		CPUTimeScopeTree::Instance().BeginFrame();
	}

	void Profiler::EndFrame()
	{
		if (!m_FrameBegun)
			return;

		m_FrameBegun = false;
		const double frame_ms = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - m_FrameStart).count();

		CPUTimeScopeTree& cpu = CPUTimeScopeTree::Instance();
		const uint32_t frame_id = cpu.GetFrameId();
		cpu.EndFrame();

		PendingFrame& pending = m_PendingFrames[frame_id % kPendingCount];
		pending.FrameId = frame_id;
		pending.FrameMs = static_cast<float>(frame_ms);

		/* 阶段值取本帧录制的原始树：等 GPU 时实时树不结算，从树里读会是上一帧 */
		const CPUTimeScopeRecorder& recorder = cpu.GetRecorder();
		const uint32_t frame_index = recorder.FindIndexByFrameId(frame_id);
		if (frame_index != CPUTimeScopeRecorder::INVALID_FRAME_INDEX)
			ReadStages(recorder.GetFrame(frame_index), pending.StageMs, pending.StageSeen);
		else
		{
			for (uint32_t stage = 0; stage < FRAME_STAGE_COUNT; ++stage)
			{
				pending.StageMs[stage] = 0.0f;
				pending.StageSeen[stage] = false;
			}
		}

		pending.Used = true;

		for (PendingFrame& frame : m_PendingFrames)
		{
			if (frame.Used && frame.FrameId + kPendingTimeout < frame_id)
				PublishFrame(frame, false, 0.0f);
		}

		if (!m_WaitForGpu && pending.Used)
			PublishFrame(pending, false, 0.0f);
	}

	void Profiler::EndGPUFrame()
	{
		GPUTimeScopeTree& gpu = GPUTimeScopeTree::Instance();
		gpu.EndFrame();

		/* 一帧的两半一起发布：GPU 结果这次解析出来后，才把同一帧的 CPU 数据放出去，
		 * 否则面板上 CPU 是当前帧、GPU 是两帧前，读起来像同一个数。 */
		const bool gpu_timing = gpu.IsEnabled();
		m_WaitForGpu = gpu_timing;
		CPUTimeScopeTree::Instance().SetWaitForGpu(gpu_timing);

		if (gpu.PrepareQueryResult())
		{
			const uint32_t gpu_frame = gpu.GetResultFrameId();
			CPUTimeScopeTree::Instance().SettleRealtimeFrame(gpu_frame);

			const GPUTimeScopeNode& root = gpu.GetRealtimeRoot();
			PublishGPU(gpu_frame, root.HasData, static_cast<float>(root.TotalUs * 0.001));
		}

		/* 上一帧的绘制已经全部提交：结算几何统计，与 GPU 计时器同一帧边界 */
		RenderStatistics::Instance().EndFrame();
	}

	void Profiler::BeginGPUFrame(uint32_t frame_id)
	{
		GPUTimeScopeTree::Instance().BeginFrame(frame_id);
	}

	void Profiler::SetCPUEnabled(bool enabled)
	{
		CPUTimeScopeTree::Instance().SetEnabled(enabled);
	}

	void Profiler::SetGPUEnabled(bool enabled)
	{
		GPUTimeScopeTree::Instance().SetEnabled(enabled);
	}

	bool Profiler::IsCPUEnabled() const
	{
		return CPUTimeScopeTree::Instance().IsEnabled();
	}

	bool Profiler::IsGPUEnabled() const
	{
		return GPUTimeScopeTree::Instance().IsEnabled();
	}

	void Profiler::SetChannelFilter(ProfileChannel channels)
	{
		CPUTimeScopeTree::Instance().SetChannelMask(channels);
	}

	ProfileChannel Profiler::GetChannelFilter() const
	{
		return CPUTimeScopeTree::Instance().GetChannelMask();
	}

	void Profiler::SetWaitForGPU(bool wait)
	{
		m_WaitForGpu = wait;
		CPUTimeScopeTree::Instance().SetWaitForGpu(wait);
	}

	const CPUTimeScopeNode& Profiler::GetCPURoot() const
	{
		return CPUTimeScopeTree::Instance().GetRealtimeRoot();
	}

	const GPUTimeScopeNode& Profiler::GetGPURoot() const
	{
		return GPUTimeScopeTree::Instance().GetRealtimeRoot();
	}

	const FrameTimeSnapshot& Profiler::GetFrameTime() const
	{
		return m_Snapshot;
	}

	uint32_t Profiler::GetFrameHistoryCount() const
	{
		return m_HistoryCount;
	}

	const FrameTimeSample& Profiler::GetFrameHistorySample(uint32_t index) const
	{
		ASSERT(index < m_HistoryCount, "Profiler history index out of range!");

		const uint32_t begin = (m_HistoryWrite + kHistorySize - m_HistoryCount) % kHistorySize;
		return m_History[(begin + index) % kHistorySize];
	}

	const RenderStatisticsSnapshot& Profiler::GetRenderStatistics() const
	{
		return RenderStatistics::Instance().GetSnapshot();
	}

	void Profiler::SetRecording(bool recording)
	{
		CPUTimeScopeRecorder& cpu = CPUTimeScopeTree::Instance().GetRecorder();
		GPUTimeScopeRecorder& gpu = GPUTimeScopeTree::Instance().GetRecorder();

		if (recording)
		{
			cpu.Resume();
			gpu.Resume();
		}
		else
		{
			cpu.Stop();
			gpu.Stop();
		}
	}

	void Profiler::SetCPURecording(bool recording)
	{
		CPUTimeScopeRecorder& recorder = CPUTimeScopeTree::Instance().GetRecorder();
		if (recording)
			recorder.Resume();
		else
			recorder.Stop();
	}

	void Profiler::SetGPURecording(bool recording)
	{
		GPUTimeScopeRecorder& recorder = GPUTimeScopeTree::Instance().GetRecorder();
		if (recording)
			recorder.Resume();
		else
			recorder.Stop();
	}

	bool Profiler::IsRecording() const
	{
		return CPUTimeScopeTree::Instance().GetRecorder().IsRecording();
	}

	void Profiler::ClearRecording()
	{
		CPUTimeScopeTree::Instance().GetRecorder().Clear();
		GPUTimeScopeTree::Instance().GetRecorder().Clear();
	}

	uint32_t Profiler::GetCPUFrameCount() const
	{
		return CPUTimeScopeTree::Instance().GetRecorder().GetFrameCount();
	}

	const CPUTimeScopeNode& Profiler::GetCPUFrame(uint32_t index) const
	{
		return CPUTimeScopeTree::Instance().GetRecorder().GetFrame(index);
	}

	uint32_t Profiler::GetCPUFrameId(uint32_t index) const
	{
		return CPUTimeScopeTree::Instance().GetRecorder().GetFrameId(index);
	}

	uint32_t Profiler::FindCPUFrame(uint32_t frame_id) const
	{
		return CPUTimeScopeTree::Instance().GetRecorder().FindIndexByFrameId(frame_id);
	}

	const GPUTimeScopeRecorder& Profiler::GetGPURecorder() const
	{
		return GPUTimeScopeTree::Instance().GetRecorder();
	}

	uint32_t Profiler::GetGPUFrameCount() const
	{
		return GPUTimeScopeTree::Instance().GetRecorder().GetFrameCount();
	}

	const GPUTimeScopeNode& Profiler::GetGPUFrame(uint32_t index) const
	{
		return GPUTimeScopeTree::Instance().GetRecorder().GetFrame(index);
	}

	uint32_t Profiler::GetGPUFrameId(uint32_t index) const
	{
		return GPUTimeScopeTree::Instance().GetRecorder().GetFrameId(index);
	}

	uint32_t Profiler::FindGPUFrame(uint32_t frame_id) const
	{
		return GPUTimeScopeTree::Instance().GetRecorder().FindIndexByFrameId(frame_id);
	}

	const char* Profiler::GetStageName(FrameStage stage)
	{
		static const char* kStageNames[FRAME_STAGE_COUNT] = { "Update", "UI", "Present" };

		const uint32_t index = static_cast<uint32_t>(stage);
		ASSERT(index < FRAME_STAGE_COUNT, "Profiler stage out of range!");
		return kStageNames[index];
	}

	uint32_t Profiler::GetFrameId() const
	{
		return CPUTimeScopeTree::Instance().GetFrameId();
	}

	uint32_t Profiler::GetGPUResultFrameId() const
	{
		return GPUTimeScopeTree::Instance().GetResultFrameId();
	}

	// 帧统计：帧间隔自己记，阶段从录制取，两半等 GPU 就绪后一起发布
	void Profiler::ReadStages(const CPUTimeScopeNode& frame,
		float (&stage_ms)[FRAME_STAGE_COUNT], bool (&seen)[FRAME_STAGE_COUNT])
	{
		for (uint32_t stage = 0; stage < FRAME_STAGE_COUNT; ++stage)
		{
			stage_ms[stage] = 0.0f;
			seen[stage] = false;
		}

		for (const CPUTimeScopeNode& child : frame.Children)
		{
			for (uint32_t stage = 0; stage < FRAME_STAGE_COUNT; ++stage)
			{
				if (child.Label == GetStageName(static_cast<FrameStage>(stage)))
				{
					stage_ms[stage] = static_cast<float>(child.TotalUs * 0.001);
					seen[stage] = true;
				}
			}
		}
	}

	Profiler::PendingFrame* Profiler::FindPending(uint32_t frame_id)
	{
		for (PendingFrame& frame : m_PendingFrames)
		{
			if (frame.Used && frame.FrameId == frame_id)
				return &frame;
		}
		return nullptr;
	}

	void Profiler::PublishGPU(uint32_t frame_id, bool gpu_valid, float gpu_ms)
	{
		PendingFrame* pending = FindPending(frame_id);
		if (pending == nullptr)
			return;

		PublishFrame(*pending, gpu_valid, gpu_ms);
	}

	void Profiler::PushFrameSample(float frame_ms, const float (&stage_ms)[FRAME_STAGE_COUNT],
		bool gpu_valid, float gpu_ms)
	{
		PendingFrame frame;
		frame.FrameMs = frame_ms;
		for (uint32_t stage = 0; stage < FRAME_STAGE_COUNT; ++stage)
		{
			frame.StageMs[stage] = stage_ms[stage];
			frame.StageSeen[stage] = stage_ms[stage] > 0.0f;
		}

		PublishFrame(frame, gpu_valid, gpu_ms);
	}

	/* 把一帧的两半一起结算：更新快照并推进历史 */
	void Profiler::PublishFrame(PendingFrame& frame, bool gpu_valid, float gpu_ms)
	{
		frame.Used = false;

		double cpu_ms = 0.0;
		for (uint32_t stage = 0; stage < FRAME_STAGE_COUNT; ++stage)
		{
			if (frame.StageSeen[stage])
				cpu_ms += frame.StageMs[stage];
		}

		const auto smooth = [this](float current, float raw)
			{
				return m_HasSnapshot ? static_cast<float>(current * 0.9 + raw * 0.1) : raw;
			};

		m_Snapshot.FrameMs = smooth(m_Snapshot.FrameMs, frame.FrameMs);
		m_Snapshot.CPUMs = smooth(m_Snapshot.CPUMs, static_cast<float>(cpu_ms));

		for (uint32_t stage = 0; stage < FRAME_STAGE_COUNT; ++stage)
		{
			/* 本帧没采到这个阶段：保留上一次的平滑值，只标"无数据"，不归零 */
			m_Snapshot.StageValid[stage] = frame.StageSeen[stage];
			if (frame.StageSeen[stage])
				m_Snapshot.StageMs[stage] = smooth(m_Snapshot.StageMs[stage], frame.StageMs[stage]);
		}

		if (gpu_valid)
			m_Snapshot.GPUMs = gpu_ms;
		m_Snapshot.GPUValid = gpu_valid;

		m_HasSnapshot = true;

		FrameTimeSample& sample = m_History[m_HistoryWrite];
		sample.FrameMs = frame.FrameMs;
		sample.CPUMs = static_cast<float>(cpu_ms);
		sample.GPUMs = gpu_ms;
		sample.GPUValid = gpu_valid;

		m_HistoryWrite = (m_HistoryWrite + 1) % kHistorySize;
		if (m_HistoryCount < kHistorySize)
			++m_HistoryCount;
	}
}
