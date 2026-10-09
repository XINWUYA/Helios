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
	};
}
