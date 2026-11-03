#type vertex
#version 410 core

/* 前向版地形：与 Terrain.glsl 同一份顶点逻辑，片元只出颜色。
 * 单独一份是因为前向与延迟的输出契约不同（8 张 G-Buffer 附件 vs 一张颜色），
 * 而引擎的材质变体还没接线（见 Shader 变体方案复评）。 */
layout(location = 0) in vec2 a_Uv;
layout(location = 5) in vec4 a_PatchInfo;
layout(location = 6) in vec4 a_PatchLod;

#include "TerrainCommon.glsl"

struct SVextex2Frag
{
	vec3 WorldPosition;
	vec2 LocalXZ;
	float Step;
	float ReceiveShadow;
};

out SVextex2Frag vert2frag;

uniform float u_TerrainReceiveShadow = 1.0f;
/* 地块实体的 id：实例化绘制一次一块地形，按 per-draw 传 */
uniform int u_TerrainObjectId = -1;

void main()
{
	float morph = 1.0f;
	vec2 local_xz;
	const vec3 world = TerrainVertexWorld(a_Uv, a_PatchInfo, a_PatchLod, morph, local_xz);

	gl_Position = u_ViewProjectionMat * vec4(world, 1.0f);
	vert2frag.WorldPosition = world;
	vert2frag.LocalXZ = local_xz;
	vert2frag.Step = max(a_PatchLod.y, 0.05f);
	vert2frag.ReceiveShadow = u_TerrainReceiveShadow;
}


#type fragment
#version 410 core

#include "TerrainCommon.glsl"
#include "TerrainMaterial.glsl"
#include "TerrainVTSample.glsl"
#include "../Builtin/Math.glsl"
#include "../Builtin/DirectLight.glsl"
#include "../Builtin/LightCompose.glsl"
#include "../DebugShaders/DebugForward.glsl"

struct SVextex2Frag
{
	vec3 WorldPosition;
	vec2 LocalXZ;
	float Step;
	float ReceiveShadow;
};

in SVextex2Frag vert2frag;

layout(location = 0) out vec4 OutFragColor;
layout(location = 1) out float ObjectId;
/* 地块实体的 id（per-draw） */
uniform int u_TerrainObjectId = -1;

void main()
{
	STerrainSurface surface = EvaluateTerrainSurface(vert2frag.LocalXZ);

	/* 页驻留就采页池，不驻留用刚算的那份 —— 与延迟管线同一套判断 */
	/* 页驻留就采页池；页没驻留而像素足迹又比底图粗，采远景底图；都不行才用刚算出来的值 */
	const vec2 cached_xz = vert2frag.WorldPosition.xz;
	const float footprint = max(length(dFdx(cached_xz)), length(dFdy(cached_xz)));
	vec4 cached_albedo;
	if (TerrainVirtualTextureLookup(cached_xz, cached_albedo)
		|| TerrainBasemapLookup(cached_xz, footprint, cached_albedo))
	{
		surface.Albedo = cached_albedo.rgb;
		surface.Roughness = cached_albedo.a;
	}

	const vec3 N = surface.Normal;
	vec3 albedo = surface.Albedo;

	/* 地形只吃主方向光一笔：前向的地形提交在逐光源循环之外，光照 UBO 已经填成主光。
	 * 不声明 u_ComposeAmbientEmission，SupportsDirectLighting 判为 false，正好走单笔路径 */
	vec3 L;
	float attenuation;
	float shadow;
	EvaluateDirectLight(u_ShadowMap, vert2frag.WorldPosition, N, L, attenuation, shadow);
	shadow *= vert2frag.ReceiveShadow;

	const float ndl = max(dot(N, L), 0.0f);
	const vec3 direct = albedo * ndl * INV_PI * LightFactor(attenuation, shadow);
	const vec3 ambient = ComposeFallbackAmbient(vec3(1.0f), albedo, 1.0f) * 0.5f;

	ObjectId = float(u_TerrainObjectId);

	/* 调试分流：读数取自本笔实际着色用的量，与正常着色同源 */
	if (u_DebugView != 0)
	{
		SMaterialInput mtl = MakeDebugMaterialInput(albedo, N, surface.Roughness, 0.0f);
		SDebugLighting lit;
		lit.DirectDiffuse = direct;
		lit.DirectSpecular = vec3(0.0f);
		lit.IndirectDiffuse = ambient;
		lit.IndirectSpecular = vec3(0.0f);
		OutFragColor = DebugForwardOutput(mtl, lit, attenuation, shadow);
		return;
	}

	OutFragColor = vec4(direct + ambient, 1.0f);
}
