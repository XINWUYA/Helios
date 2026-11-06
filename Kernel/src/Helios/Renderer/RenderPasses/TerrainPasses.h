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

		/* GPU 选择：注册一个计算 Pass，按本视图的视锥选 patch 并压实，写好间接参数。
		 * 必须排在地形绘制之前注册，也要排在所有渲染 Pass 之前 —— 计算派发会把
		 * 当前渲染编码器收掉，夹在渲染通道中间会把它们切碎 */
		HELIOS_API void AddSelectPass(RenderView& view);

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

		/* 虚拟纹理烘焙：注册一个渲染 Pass，页池纹理作为颜色附件进图，
		 * 执行时挑出本帧缺的页逐页烘进去。要排在用虚拟纹理的绘制之前 */
		HELIOS_API void AddVirtualTextureBakePass(RenderView& view);

		/* 探针捕获：按探针周围的一块范围选 patch，不受主相机视锥限制。
		 * culling_mask 取探针自己的，不能用宿主相机的（两者可以不一样） */
		HELIOS_API void SubmitCapture(RenderView& view, Scene& scene, const glm::vec3& center,
			float radius, LayerMask culling_mask);
	}
}
