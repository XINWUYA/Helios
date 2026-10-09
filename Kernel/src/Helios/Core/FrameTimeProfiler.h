#pragma once
#include <chrono>
#include <cstdint>

namespace Helios
{
	/* 每帧 CPU 耗时的轻量统计（主循环写、统计面板读）。口径：Frame = BeginFrame → EndFrame
	 * （含帧准备里的垂直同步等待）；CPU = Update + UI + Present 之和（不含等待，才看得出余量）；
	 * GPU = 本帧总耗时（调用方在 EndFrame 传入）。平滑按 0.9 / 0.1 EMA；历史保留最近 kHistorySize
	 * 帧的原始值供面板画曲线。 */
	class FrameTimeProfiler
	{
	public:
		/* CPU 阶段划分（不渲染的帧不统计） */
		enum class Stage : uint8_t
		{
			Update = 0,  /* 逻辑更新与渲染提交（Renderer::Update + 各 Layer::OnUpdate） */
			UI,          /* ImGui：界面更新（OnImGuiRender）与绘制数据生成 */
			Present,     /* 交换 / 提交显示（含事件轮询） */
			Count
		};

		static constexpr uint32_t STAGE_COUNT = static_cast<uint32_t>(Stage::Count);
		static constexpr uint32_t kHistorySize = 120;

		/* 阶段的显示名（面板 / 日志用） */
		static const char* GetStageName(Stage stage);

		/* 当前帧的统计快照（平滑值） */
		struct Snapshot
		{
			float FrameMs{ 0.0f };           /* 帧间隔 */
			float CpuMs{ 0.0f };             /* 主线程工作合计 */
			float StageMs[STAGE_COUNT]{};    /* 各阶段耗时 */
			float GpuMs{ 0.0f };             /* GPU 总耗时（来自计时器根节点，已平滑） */
			bool GpuValid{ false };          /* GPU 计时是否可用（未启用 / 无数据为 false） */
		};

		/* 历史样本（原始值，供曲线） */
		struct Sample
		{
			float FrameMs{ 0.0f };
			float CpuMs{ 0.0f };
			float GpuMs{ 0.0f };
			bool GpuValid{ false };
		};

		static FrameTimeProfiler& Instance();

		/* 帧边界：主循环最外层每帧一对（最小化等不渲染的帧不调用 BeginFrame，
		 * 对应的 EndFrame 会被丢弃） */
		void BeginFrame();
		/* 结算本帧并推进历史；gpu_ms 来自 GPU 计时器根节点，无数据时 gpu_valid=false */
		void EndFrame(bool gpu_valid, float gpu_ms);

		/* 累积一个阶段的耗时（毫秒；由 ScopedFrameStage 自动调用） */
		void AddStageTime(Stage stage, double milliseconds);

		[[nodiscard]] const Snapshot& GetSnapshot() const { return m_Snapshot; }

		/* 历史样本：index 0 = 最旧；越界断言 */
		[[nodiscard]] uint32_t GetHistoryCount() const { return m_HistoryCount; }
		[[nodiscard]] const Sample& GetHistorySample(uint32_t index) const;

	private:
		FrameTimeProfiler() = default;

		Snapshot m_Snapshot{};
		bool m_HasSnapshot{ false };  /* 首帧直接落值（EMA 从 0 起爬会明显滞后） */
		bool m_FrameBegun{ false };   /* 本帧是否已 BeginFrame（最小化丢弃用） */

		std::chrono::steady_clock::time_point m_FrameStart{};
		double m_PendingStageMs[STAGE_COUNT]{};

		Sample m_History[kHistorySize]{};
		uint32_t m_HistoryWrite{ 0 }; /* 下一个写入槽位 */
		uint32_t m_HistoryCount{ 0 };
	};

	/* 一个 CPU 阶段的 RAII 计时：作用域结束时把耗时累加进 FrameTimeProfiler */
	class ScopedFrameStage
	{
	public:
		explicit ScopedFrameStage(FrameTimeProfiler::Stage stage);
		~ScopedFrameStage();

		ScopedFrameStage(const ScopedFrameStage&) = delete;
		ScopedFrameStage& operator=(const ScopedFrameStage&) = delete;

	private:
		FrameTimeProfiler::Stage m_Stage;
		std::chrono::steady_clock::time_point m_Start;
	};
}
