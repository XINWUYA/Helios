#include "Pch.h"
#include "FrameTimeProfiler.h"

namespace Helios
{
	namespace
	{
		/* 平滑系数（与 GPU 计时树一致）：新值占 0.1 */
		constexpr double SmoothKeep = 0.9;
		constexpr double SmoothNew = 0.1;

		[[nodiscard]] double ElapsedMilliseconds(
			const std::chrono::steady_clock::time_point& begin,
			const std::chrono::steady_clock::time_point& end)
		{
			return std::chrono::duration<double, std::milli>(end - begin).count();
		}
	}

	FrameTimeProfiler& FrameTimeProfiler::Instance()
	{
		static FrameTimeProfiler instance;
		return instance;
	}

	const char* FrameTimeProfiler::GetStageName(Stage stage)
	{
		static const char* kStageNames[STAGE_COUNT] = { "Update", "UI", "Present" };

		const uint32_t index = static_cast<uint32_t>(stage);
		ASSERT(index < STAGE_COUNT, "FrameTimeProfiler stage out of range!");
		return kStageNames[index];
	}

	void FrameTimeProfiler::BeginFrame()
	{
		PROFILE_FUNCTION();

		m_FrameBegun = true;
		m_FrameStart = std::chrono::steady_clock::now();
		for (double& stage_ms : m_PendingStageMs)
			stage_ms = 0.0;
	}

	void FrameTimeProfiler::EndFrame(bool gpu_valid, float gpu_ms)
	{
		PROFILE_FUNCTION();

		if (!m_FrameBegun)
			return; /* 最小化等没有 BeginFrame 的帧：丢弃（Present 阶段在循环尾照常计时） */

		m_FrameBegun = false;

		const double frame_ms = ElapsedMilliseconds(m_FrameStart, std::chrono::steady_clock::now());

		double cpu_ms = 0.0;
		for (const double stage_ms : m_PendingStageMs)
			cpu_ms += stage_ms;

		const auto smooth = [this](float current, double raw)
			{
				if (!m_HasSnapshot)
					return static_cast<float>(raw);
				return static_cast<float>(current * SmoothKeep + raw * SmoothNew);
			};

		m_Snapshot.FrameMs = smooth(m_Snapshot.FrameMs, frame_ms);
		m_Snapshot.CpuMs = smooth(m_Snapshot.CpuMs, cpu_ms);
		for (uint32_t stage = 0; stage < STAGE_COUNT; ++stage)
			m_Snapshot.StageMs[stage] = smooth(m_Snapshot.StageMs[stage], m_PendingStageMs[stage]);

		/* GPU 值来自计时器根节点（树上已做同样的平滑），不在这里二次平滑，直接落值 */
		if (gpu_valid)
			m_Snapshot.GpuMs = gpu_ms;
		m_Snapshot.GpuValid = gpu_valid;

		m_HasSnapshot = true;

		Sample& sample = m_History[m_HistoryWrite];
		sample.FrameMs = static_cast<float>(frame_ms);
		sample.CpuMs = static_cast<float>(cpu_ms);
		sample.GpuMs = gpu_ms;
		sample.GpuValid = gpu_valid;

		m_HistoryWrite = (m_HistoryWrite + 1) % kHistorySize;
		if (m_HistoryCount < kHistorySize)
			++m_HistoryCount;
	}

	void FrameTimeProfiler::AddStageTime(Stage stage, double milliseconds)
	{
		m_PendingStageMs[static_cast<uint32_t>(stage)] += milliseconds;
	}

	const FrameTimeProfiler::Sample& FrameTimeProfiler::GetHistorySample(uint32_t index) const
	{
		ASSERT(index < m_HistoryCount, "FrameTimeProfiler history index out of range!");

		const uint32_t begin = (m_HistoryWrite + kHistorySize - m_HistoryCount) % kHistorySize;
		return m_History[(begin + index) % kHistorySize];
	}

	ScopedFrameStage::ScopedFrameStage(FrameTimeProfiler::Stage stage)
		: m_Stage(stage)
		, m_Start(std::chrono::steady_clock::now())
	{
	}

	ScopedFrameStage::~ScopedFrameStage()
	{
		FrameTimeProfiler::Instance().AddStageTime(m_Stage,
			ElapsedMilliseconds(m_Start, std::chrono::steady_clock::now()));
	}
}
