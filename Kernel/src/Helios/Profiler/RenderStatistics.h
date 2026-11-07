#pragma once
#include <cstdint>
#include "Helios/Profiler/Profiler.h"
#include "Helios/Renderer/RenderCommon.h"

namespace Helios
{
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

		/* 草地的一笔提交：不参与几何量折算，只单独记笔数 */
		void AddGrassDrawCall();

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
