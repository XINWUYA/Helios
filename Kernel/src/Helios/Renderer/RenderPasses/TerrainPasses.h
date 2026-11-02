#pragma once
#include <cstdint>
#include <glm/glm.hpp>

#include "Helios/Common/Export.h"
#include "Helios/Renderer/RenderCommon.h"
#include "Helios/Scene/LayerMask.h"

namespace Helios
{
	class Material;
	class RenderView;
	class Scene;

	/* 地形提交：地形不是 MeshSegment，不进可见对象列表，由各 Pass 在常规对象之后补一趟。
	 * 延迟管线写 G-Buffer，前向管线单笔主方向光，阴影 / 速度 / 探针 / 调试各补各的。 */
	namespace TerrainDraw
	{
		/* 场景里有没有本视图可见的地形（延迟管线据此决定要不要补 G-Buffer 那一趟） */
		HELIOS_API bool HasVisibleTerrain(const Scene& scene, LayerMask culling_mask);

		/* 延迟管线：在 G-Buffer pass 内、常规对象画完之后调用，附件已绑好 */
		HELIOS_API void SubmitGBuffer(RenderView& view, Scene& scene);

		/* 前向管线：在场景对象之后调用，光照 UBO 已填成主方向光。
		 * debug_view 给定时按这一档出读数（阴影累加那趟显式传 Shadow） */
		HELIOS_API void SubmitForward(RenderView& view, Scene& scene,
			DebugViewMode debug_view = DebugViewMode::None);

		/* 调试重绘：Overdraw / Mipmap / Wireframe 这些要重画几何的档位 */
		HELIOS_API void SubmitDebug(RenderView& view, Scene& scene, const SharedPtr<Material>& material);

		/* 方向光级联的投影：patch 按该级联的盒子选，矩阵走光照 UBO 的级联槽位 */
		HELIOS_API void SubmitShadow(RenderView& view, Scene& scene,
			const glm::mat4& light_view_projection, uint8_t cascade_id);

		/* 前向管线的运动矢量：地形是静止的，位移全部来自相机 */
		HELIOS_API void SubmitVelocity(RenderView& view, Scene& scene);

		/* 虚拟纹理：挑出本帧要烘的页并烘进页池（切自己的帧缓冲，调用方在绑主通道之前调） */
		HELIOS_API void BakeVirtualTextures(RenderView& view, Scene& scene);

		/* 探针捕获：按探针周围的一块范围选 patch，不受主相机视锥限制 */
		HELIOS_API void SubmitCapture(RenderView& view, Scene& scene, const glm::vec3& center, float radius);
	}
}
