#pragma once

namespace Helios
{
	/* 「Render Stats」面板：每帧 CPU / GPU 性能统计。两张卡 —— Frame（FPS / 帧间隔 / CPU 阶段 /
	 * GPU 合计 + 三线历史曲线）和 GPU Timings（录制 / 逐帧回放 / 导出 CSV + 耗时树）。数据源 =
	 * FrameTimeProfiler + RenderQueryProfiler。（todo：3D 统计等数据源就绪再补一张。） */
	class RenderStatsPanel
	{
	public:
		/* 画整个面板（含停靠窗口的 Begin/End） */
		void OnImGuiRender();

	private:
		void ShowFrameStatsCard();
		void ShowGPUTimingsCard();
		/* 帧耗时曲线：三线（Frame / CPU / GPU）+ 悬停读值；需要至少两帧历史才画线 */
		void ShowFrameTimeGraph(float height);
		/* 录制工具栏：录制 / 停止、上一帧 / 下一帧、峰值 / 谷值、清空 —— 一行等大图标按钮
		 * （录制视图常显；实时视图只在录制进行中显示，避免录制状态失联） */
		void ShowGPUTimerToolbar();
		/* GPU 录制的逐帧回放：帧间曲线 + 选帧滑条 + 概要行；
		 * 有录制内容且不在录制中时显示，改动 m_ReplayFrame（-1 = 未选中） */
		void ShowGPUTimerRecordingUI();

		/* 回放选中的录制帧（-1 = 未选中，落到实时树） */
		int m_ReplayFrame{ -1 };

		/* GPU Timings 卡的观测方式：false = 实时树 / true = 录制视图（录制 + 逐帧回放） */
		bool m_RecordView{ false };
		/* 滑动页签的选中块位置（0 = 实时页签、1 = 录制页签）：指数趋近产生滑动动画 */
		float m_TabSlide{ 0.0f };
		/* 上一帧是否在录制中（用于"录制刚结束 → 停在最后一帧"） */
		bool m_WasRecording{ false };
	};
}
