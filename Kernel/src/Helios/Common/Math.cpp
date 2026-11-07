#include "Pch.h"
#include "Math.h"

namespace Helios
{

/* 根据指定变换恢复出平移、旋转和缩放 */
bool DecomposeTransform(const glm::mat4& transform, glm::vec3& translation, glm::vec3& rotation, glm::vec3& scale)
{
	PROFILE_CPU_FUNCTION();

	/* Ref: glm::decompose() */

	glm::mat4 transform_t = transform;
	/* 判断矩阵有效 */
	if (glm::epsilonEqual(transform_t[3][3], 0.0f, glm::epsilon<float>()))
		return false;

	/* 先处理变换矩阵最后一行，不考虑仿射变换 */
	if (glm::epsilonNotEqual(transform_t[0][3], 0.0f, glm::epsilon<float>()) ||
		glm::epsilonNotEqual(transform_t[1][3], 0.0f, glm::epsilon<float>()) ||
		glm::epsilonNotEqual(transform_t[2][3], 0.0f, glm::epsilon<float>()))
	{
		/* 清除Perspective部分 */
		transform_t[0][3] = 0.0f;
		transform_t[1][3] = 0.0f;
		transform_t[2][3] = 0.0f;
		transform_t[3][3] = 1.0f;
	}

	/* 获取translation */
	translation = glm::vec3(transform_t[3]);

	/* 清空变换矩阵translation信息 */
	transform_t[3] = glm::vec4(0.0f, 0.0f, 0.0f, transform_t[3][3]);

	glm::vec3 row[3];
	for (glm::length_t i = 0; i < 3; ++i)
		for (glm::length_t j = 0; j < 3; ++j)
			row[i][j] = transform_t[i][j];

	/* 获取scale */
	scale.x = glm::length(row[0]);
	row[0] = glm::normalize(row[0]);
	scale.y = glm::length(row[1]);
	row[1] = glm::normalize(row[1]);
	scale.z = glm::length(row[2]);
	row[2] = glm::normalize(row[2]);

	/* 获取rotation */
	rotation.y = asin(-row[0][2]);
	if (cos(rotation.y) != 0.0f) {
		rotation.x = atan2(row[1][2], row[2][2]);
		rotation.z = atan2(row[0][1], row[0][0]);
	}
	else {
		rotation.x = atan2(-row[2][0], row[1][1]);
		rotation.z = 0.0f;
	}

#if 0
	/* 列优先改为行优先 */
	glm::mat3 mat = transpose(glm::mat3(transform_t));

	/* 获取scale */
	scale = glm::vec3(glm::length(mat[0]), glm::length(mat[1]), glm::length(mat[2]));

	mat[0] = glm::detail::scale(mat[0], 1.0f);
	mat[1] = glm::detail::scale(mat[1], 1.0f);
	mat[2] = glm::detail::scale(mat[2], 1.0f);

	/* 获取rotation */
	rotation.y = asin(-mat[0][2]);
	if (cos(rotation.y) != 0.0f)
	{
		rotation.x = atan2(mat[1][2], mat[2][2]);
		rotation.z = atan2(mat[0][1], mat[0][0]);
	}
	else
	{
		rotation.x = atan2(-mat[2][0], mat[1][1]);
		rotation.z = 0;
	}
#endif

	return true;
}
}