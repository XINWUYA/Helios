#pragma once
#include <glm/glm.hpp>
#include "Helios/Common/Export.h"

namespace Helios
{
	struct VisibleMeshObject;

	/* 视锥的 6 个裁剪空间平面（ZO 深度约定：0 <= z <= w），相机与点光 / 聚光的阴影面共用。
	 * 近远面一起进判定：大世界里视距之外、光源范围之外的物体要能整批剔掉，四个侧面做不到；
	 * 正交投影下也只有近远面能封口（侧面只是无限长的方柱）。
	 * 透视投影下 w = -z，视点背后 w 为负，|x| <= w 天然不成立，侧面顺带排除了背后。 */
	struct HELIOS_API Frustum
	{
		glm::vec4 Planes[6]{};
		/* 生成它的视图投影矩阵：算视锥的世界包围盒（空间索引要查格子范围）用 */
		glm::mat4 ViewProjection{ 1.0f };

		static Frustum FromViewProjection(const glm::mat4& view_projection);
		/* 包围盒整块落在某个面外才算看不见，贴边的照画；没有包围盒或不参与剔除的恒可见 */
		[[nodiscard]] bool IsOutside(const VisibleMeshObject& object) const;
		[[nodiscard]] bool IsOutside(const glm::vec3& center, const glm::vec3& extent) const;
		/* 视锥的世界空间包围盒（8 个角点求包围，不区分深度朝向） */
		void GetWorldBounds(glm::vec3& bounds_min, glm::vec3& bounds_max) const;
	};
}
