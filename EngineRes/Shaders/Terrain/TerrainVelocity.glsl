#type vertex
#version 410 core

/* 地形的运动矢量：地形是静止的，同一个世界点用两帧的视图投影算位移，
 * 位移全部来自相机。顶点位置跟可见着色共用 TerrainCommon 的那一份。 */
layout(location = 0) in vec2 a_Uv;
layout(location = 5) in vec4 a_PatchInfo;
layout(location = 6) in vec4 a_PatchLod;

#include "../MaterialShaders/TerrainCommon.glsl"

out vec4 v_CurClip;
out vec4 v_PrevClip;

void main()
{
	float morph = 1.0f;
	const vec3 world = TerrainVertexWorld(a_Uv, a_PatchInfo, a_PatchLod.y, morph);
	v_CurClip = u_VelocityViewProjectionMat * vec4(world, 1.0f);
	v_PrevClip = u_PrevVelocityViewProjectionMat * vec4(world, 1.0f);
	/* 光栅化照旧用带抖动的矩阵，深度才跟场景写的那张对得上 */
	gl_Position = u_ViewProjectionMat * vec4(world, 1.0f);
}


#type fragment
#version 410 core

#include "../Builtin/Velocity.glsl"

in vec4 v_CurClip;
in vec4 v_PrevClip;

layout(location = 0) out vec2 OutVelocity;

void main()
{
	OutVelocity = ComputeVelocityFromClip(v_CurClip, v_PrevClip);
}
