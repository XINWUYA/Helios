#pragma once
#include <glm/glm.hpp>

namespace Helios
{
constexpr float PI = 3.14159265359f;
constexpr float INV_PI = 0.31830988618f;

/* 引擎深度约定（CPU 和 GPU 一致）：ZO = NDC/深度 z ∈ [0,1]（跟 Metal 匹配，GL 后端经
 * glClipControl 对齐）；Reversed-Z = 近 1 / 远 0，比较默认 GreaterEqual。ZO 下不能简单翻 z 列
 * 符号（会被近平面裁掉），要按矩阵类型推：透视 [2][2] = -(p[2][2]+1)、[3][2] = -p[3][2]；
 * 正交 [2][2] = -p[2][2]、[3][2] = 1 - p[3][2]。 */
[[nodiscard]] inline glm::mat4 MakeReversedZProjection(const glm::mat4& proj)
{
	glm::mat4 reversed = proj;
	if (proj[3][3] == 0.0f) /* 透视投影 */
	{
		reversed[2][2] = -(proj[2][2] + 1.0f);
		reversed[3][2] = -proj[3][2];
	}
	else /* 正交投影 */
	{
		reversed[2][2] = -proj[2][2];
		reversed[3][2] = 1.0f - proj[3][2];
	}
	return reversed;
}

/* 根据指定变换恢复出平移、旋转和缩放 */
bool DecomposeTransform(const glm::mat4& transform, glm::vec3& translation, glm::vec3& rotation, glm::vec3& scale);

}