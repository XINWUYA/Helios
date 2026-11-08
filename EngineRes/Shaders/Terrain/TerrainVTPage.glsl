#type vertex
#version 410 core

/* 页烘焙：一整页就是一个全屏四边形，世界坐标由页矩形推出来 */
layout(location = 0) in vec4 a_Position;

out vec2 v_Uv;

void main()
{
	v_Uv = a_Position.xy * 0.5f + 0.5f;
	gl_Position = a_Position;
}


#type fragment
#version 410 core

#include "TerrainMaterial.glsl"

in vec2 v_Uv;

layout(location = 0) out vec4 OutAlbedo; /* rgb: 底色；a: 粗糙度 */

/* 本页在世界里的左下角（x, z）与页宽（米，在 w） */
uniform vec4 u_VTPageRect = vec4(0.0f, 0.0f, 64.0f, 0.0f);

void main()
{
	const vec2 world_xz = u_VTPageRect.xz + v_Uv * u_VTPageRect.w;
	/* 页是按世界矩形切的（与可见着色的查表同一套），算外观前拉回地形局部 */
	const vec2 local_xz = (u_TerrainInverse * vec4(world_xz.x, u_TerrainOrigin.y, world_xz.y, 1.0f)).xz;

	const STerrainSurface surface = EvaluateTerrainSurface(local_xz);
	OutAlbedo = vec4(surface.Albedo, surface.Roughness);
}
