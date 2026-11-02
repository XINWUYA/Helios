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

#include "../MaterialShaders/TerrainMaterial.glsl"

in vec2 v_Uv;

layout(location = 0) out vec4 OutAlbedo; /* rgb: 底色；a: 粗糙度 */

/* 本页在世界里的左下角（x, z）与页宽（米） */
uniform vec4 u_VTPageRect = vec4(0.0f, 0.0f, 64.0f, 0.0f);

void main()
{
	const vec2 world_xz = u_VTPageRect.xz + v_Uv * u_VTPageRect.z;

	/* 差分用页纹素的跨度：页是固定分辨率，跟屏幕上的 LOD 无关 */
	const STerrainSurface surface = EvaluateTerrainSurface(world_xz, u_VTPageRect.z / 256.0f);
	OutAlbedo = vec4(surface.Albedo, surface.Roughness);
}
