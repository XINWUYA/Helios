#include "Pch.h"
#include "RenderStatistics.h"

namespace Helios
{
	uint64_t CountTriangles(PrimitiveType type, uint64_t element_count)
	{
		switch (type)
		{
		case PrimitiveType::Triangles:
			return element_count / 3;
		/* 三角带：够三个元素才出首块，之后每多一个元素多一个三角形 */
		case PrimitiveType::Triangle_Strip:
			return element_count > 2 ? element_count - 2 : 0;
		default:
			return 0;
		}
	}

	RenderStatistics& RenderStatistics::Instance()
	{
		static RenderStatistics instance;
		return instance;
	}

	void RenderStatistics::AddDrawCall(PrimitiveType type, uint64_t element_count, uint32_t instances, bool indirect)
	{
		++m_Pending.DrawCalls;

		/* 间接绘制只知道有一笔，元素与实例数都在参数块里由 GPU 读 */
		if (indirect)
		{
			++m_Pending.IndirectDrawCalls;
			return;
		}

		const uint64_t instance_count = instances > 0 ? instances : 1;
		m_Pending.Vertices += element_count * instance_count;
		m_Pending.Triangles += CountTriangles(type, element_count) * instance_count;
		m_Pending.Instances += instances;
	}

	void RenderStatistics::AddGrassDrawCall(GrassPass pass)
	{
		switch (pass)
		{
		case GrassPass::Visible:
			++m_Pending.GrassVisibleDrawCalls;
			break;
		case GrassPass::Shadow:
			++m_Pending.GrassShadowDrawCalls;
			break;
		case GrassPass::Velocity:
			++m_Pending.GrassVelocityDrawCalls;
			break;
		}
	}

	void RenderStatistics::AddUIDrawCall(uint64_t vertices)
	{
		++m_Pending.UIDrawCalls;
		m_Pending.UIVertices += vertices;
	}

	void RenderStatistics::AddUIUpdateMs(float milliseconds)
	{
		m_Pending.UIUpdateMs += milliseconds;
	}

	void RenderStatistics::AddUIRenderMs(float milliseconds)
	{
		m_Pending.UIRenderMs += milliseconds;
	}

	void RenderStatistics::SetUIFontAtlasBytes(uint64_t bytes)
	{
		m_Pending.UIFontAtlasBytes = bytes;
	}

	void RenderStatistics::EndFrame()
	{
		m_Snapshot = m_Pending;
		m_Snapshot.Valid = true;
		m_Pending = {};
	}

	void RenderStatistics::Reset()
	{
		m_Snapshot = {};
		m_Pending = {};
	}
}
