#pragma once
#include "Helios/Common/Export.h"
#include "Helios/Scene/LayerMask.h"

namespace Helios
{
	class RenderView;
	class Scene;

	/* 地形提交：地形不是 MeshSegment，不进可见对象列表，由各 Pass 在常规对象之后补一趟。
	 * 延迟管线写 G-Buffer，前向管线单笔主方向光。 */
	namespace TerrainDraw
	{
		/* 场景里有没有本视图可见的地形（延迟管线据此决定要不要补 G-Buffer 那一趟） */
		HELIOS_API bool HasVisibleTerrain(const Scene& scene, LayerMask culling_mask);

		/* 延迟管线：在 G-Buffer pass 内、常规对象画完之后调用，附件已绑好 */
		HELIOS_API void SubmitGBuffer(RenderView& view, Scene& scene);

		/* 前向管线：在场景对象之后调用，光照 UBO 已填成主方向光 */
		HELIOS_API void SubmitForward(RenderView& view, Scene& scene);
	}
}
