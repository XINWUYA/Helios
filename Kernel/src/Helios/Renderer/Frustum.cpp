#include "Frustum.h"

#include <limits>
#include "RenderView.h"
#include <Helios/Scene/Mesh.h>

namespace Helios
{
	Frustum Frustum::FromViewProjection(const glm::mat4& view_projection)
	{
		/* glm 按列存，第 i 行 = (m[0][i], m[1][i], m[2][i], m[3][i]) */
		const auto row = [&view_projection](int index)
		{
			return glm::vec4(view_projection[0][index], view_projection[1][index],
				view_projection[2][index], view_projection[3][index]);
		};
		const glm::vec4 row_x = row(0);
		const glm::vec4 row_y = row(1);
		const glm::vec4 row_z = row(2);
		const glm::vec4 row_w = row(3);
		/* 侧面 |x| <= w、|y| <= w；深度按 ZO 的 0 <= z <= w（Reversed-Z 只是近远两端对调，
		 * 围出来的还是这块体积），所以两个深度面一起加上不用管约定 */
		Frustum frustum;
		frustum.Planes[0] = row_w + row_x;
		frustum.Planes[1] = row_w - row_x;
		frustum.Planes[2] = row_w + row_y;
		frustum.Planes[3] = row_w - row_y;
		frustum.Planes[4] = row_z;
		frustum.Planes[5] = row_w - row_z;
		frustum.ViewProjection = view_projection;
		return frustum;
	}

	bool Frustum::IsOutside(const VisibleMeshObject& object) const
	{
		if (!object.Cullable)
			return false;

		glm::vec3 center = object.WorldCenter;
		glm::vec3 extent = object.WorldExtent;
		if (!object.HasWorldBounds)
		{
			/* 没在收集时量过（手工构造的对象）：现算一次，局部盒变到世界 ——
			 * 中心直接变换，半长按矩阵各轴的绝对值展开 */
			if (object.MeshSegment == nullptr)
				return false;

			const glm::vec3& local_min = object.MeshSegment->GetAABBMin();
			const glm::vec3& local_max = object.MeshSegment->GetAABBMax();
			/* 没设过包围盒（全零）当作未知，不剔 */
			if (local_min == glm::vec3(0.0f) && local_max == glm::vec3(0.0f))
				return false;

			const glm::mat4& matrix = object.Local2WorldMat;
			const glm::vec3 half = (local_max - local_min) * 0.5f;
			center = glm::vec3(matrix * glm::vec4((local_min + local_max) * 0.5f, 1.0f));
			extent = glm::abs(glm::vec3(matrix[0])) * half.x
				+ glm::abs(glm::vec3(matrix[1])) * half.y
				+ glm::abs(glm::vec3(matrix[2])) * half.z;
		}

		return IsOutside(center, extent);
	}

	bool Frustum::IsOutside(const glm::vec3& center, const glm::vec3& extent) const
	{
		for (const glm::vec4& plane : Planes)
		{
			/* 平面没归一化也没关系：只看符号，支撑函数 n·c + |n|·e 照样对 */
			const glm::vec3 normal(plane);
			if (glm::dot(normal, center) + plane.w + glm::dot(glm::abs(normal), extent) < 0.0f)
				return true;
		}
		return false;
	}

	void Frustum::GetWorldBounds(glm::vec3& bounds_min, glm::vec3& bounds_max) const
	{
		const glm::mat4 inverse = glm::inverse(ViewProjection);
		bounds_min = glm::vec3(std::numeric_limits<float>::max());
		bounds_max = glm::vec3(-std::numeric_limits<float>::max());

		/* 两个深度端点都取：Reversed-Z 下 0 是远、1 是近，八个角点合起来是同一块体积 */
		for (int depth = 0; depth <= 1; ++depth)
		{
			for (int y = -1; y <= 1; y += 2)
			{
				for (int x = -1; x <= 1; x += 2)
				{
					const glm::vec4 clip = inverse * glm::vec4(static_cast<float>(x), static_cast<float>(y),
						static_cast<float>(depth), 1.0f);
					if (std::abs(clip.w) < 1e-6f)
						continue;

					const glm::vec3 world = glm::vec3(clip) / clip.w;
					bounds_min = glm::min(bounds_min, world);
					bounds_max = glm::max(bounds_max, world);
				}
			}
		}
	}
}
