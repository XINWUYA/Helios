#type vertex
#version 410 core

/* 地形投影：顶点跟可见着色共用 TerrainCommon 的那一份，只写深度。
 * 矩阵按级联下标从光照 UBO 取 —— 全局视图矩阵是相机的，阴影这一趟不能用它。 */
layout(location = 0) in vec2 a_Uv;
layout(location = 5) in vec4 a_PatchInfo;
layout(location = 6) in vec4 a_PatchLod;

uniform int u_CascadeIndex = 0;

#include "../MaterialShaders/TerrainCommon.glsl"

void main()
{
	float morph = 1.0f;
	const vec3 world = TerrainVertexWorld(a_Uv, a_PatchInfo, a_PatchLod.y, morph);
	gl_Position = u_LightViewProjectionMat[u_CascadeIndex] * vec4(world, 1.0f);
}


#type fragment
#version 410 core

void main()
{
	/* 空片元：深度在光栅化阶段自动写入 */
}
