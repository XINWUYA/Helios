#pragma once
#include <cstdint>
#include "RenderCommon.h"

namespace Helios
{
	/* 一帧的渲染统计快照：提交次数与几何吞吐量（面板只读，不参与任何渲染决策）。
	 * 口径 —— 顶点 / 三角形按“这一笔提交了多少元素”折算，实例化乘实例数；
	 * 间接绘制（草地）的元素与实例数在 GPU 侧，只计入调用数，几何量不计。 */
	struct HELIOS_API RenderStatisticsSnapshot
	{
		uint64_t DrawCalls{ 0 };         /* 绘制调用数（普通 + 实例化 + 间接） */
		uint64_t Triangles{ 0 };         /* 三角形数 */
		uint64_t Vertices{ 0 };          /* 顶点数（索引绘制按索引数，即顶点着色器的调用量） */
		uint64_t Instances{ 0 };         /* 实例化绘制覆盖的实例总数 */
		uint64_t IndirectDrawCalls{ 0 }; /* 其中的间接绘制数 */
		/* 是否结算过至少一帧：刚启动 / 释放后为 false，面板据此显示占位 */
		bool Valid{ false };
	};

	/* 元素数折算成三角形数（纯函数，headless 可断言）：非三角形图元记 0 */
	[[nodiscard]] HELIOS_API uint64_t CountTriangles(PrimitiveType type, uint64_t element_count);

	/* 渲染统计采集：SubmitDraw 里逐笔累加，Renderer::Update 结算上一帧 ——
	 * 与 GPU 计时器同一个帧边界，两者的数字能直接对着看。 */
	class HELIOS_API RenderStatistics
	{
	public:
		static RenderStatistics& Instance();

		/* 一笔绘制：instances 传实例化绘制的实例数（非实例化传 0） */
		void AddDrawCall(PrimitiveType type, uint64_t element_count, uint32_t instances, bool indirect);

		/* 帧边界：累计值结算成快照，随后清零 */
		void EndFrame();

		[[nodiscard]] const RenderStatisticsSnapshot& GetSnapshot() const { return m_Snapshot; }

		/* 清空累计与快照（释放渲染器时） */
		void Reset();

	private:
		RenderStatistics() = default;

		RenderStatisticsSnapshot m_Snapshot{};
		RenderStatisticsSnapshot m_Pending{};
	};
}
