#pragma once

namespace Helios
{
	class RenderView;
	namespace Deferred
	{
		/* 组织延迟管线的全流程：G-Buffer → 光照 → 天空背景；渲染图重置后调用一次（默认路径由
		 * RenderView::UpdateFrameGraph 按管线选择调用）。阴影前置、材质转换、探针选择都在这里；
		 * 输出落成视图的 RenderTarget、中间资源落 Blackboard 供后续 Pass 复用。 */
		void AddDeferredPasses(RenderView* render_view);
	}
}
