#type vertex
#version 410 core

layout(location = 0) in vec3 a_Position;

/* 阴影层矩阵选择：>= 0 = 级联下标（取 u_LightViewProjectionMat 对应槽位）；-1 = 点光 / 聚光
 * 单面（取 u_ShadowPassMat，CPU 端每层渲染前下发）。裸 uniform 不能与 UBO 成员重名 ——
 * 会聚合进默认块、两块撞名编译失败 */
layout(location = 0) uniform int u_CascadeIndex;
layout(location = 1) uniform mat4 u_ShadowPassMat;

#include "Uniforms.glsl"

void main()
{
	const mat4 light_view_projection_mat = (u_CascadeIndex >= 0)
		? u_LightViewProjectionMat[u_CascadeIndex]
		: u_ShadowPassMat;
	gl_Position = light_view_projection_mat * u_Local2WorldMat * vec4(a_Position, 1.0f);
}


#type fragment
#version 410 core

void main()
{
	/* 空片元：深度在光栅化阶段自动写入，无颜色输出 */
}
