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
	float Step;
	float ReceiveShadow;
};

out SVextex2Frag vert2frag;

uniform float u_TerrainReceiveShadow = 1.0f;

void main()
{
	float morph = 1.0f;
	const vec3 world = TerrainVertexWorld(a_Uv, a_PatchInfo, a_PatchLod.y, morph);

	gl_Position = u_ViewProjectionMat * vec4(world, 1.0f);
	vert2frag.WorldPosition = world;
	const float step = max(a_PatchLod.y, 0.05f);
	vert2frag.Step = mix(step * 2.0f, step, morph);
	vert2frag.ReceiveShadow = u_TerrainReceiveShadow;
}


#type fragment
#version 410 core

#include "TerrainCommon.glsl"
#include "../Builtin/Math.glsl"
#include "../Builtin/DirectLight.glsl"
#include "../Builtin/LightCompose.glsl"
#include "../DebugShaders/DebugForward.glsl"

struct SVextex2Frag
{
	vec3 WorldPosition;
	float Step;
	float ReceiveShadow;
};

in SVextex2Frag vert2frag;

layout(location = 0) out vec4 OutFragColor;
/* 地形不参与拾取 */
layout(location = 1) out float ObjectId;

uniform vec3 u_GrassColor = vec3(0.26f, 0.36f, 0.16f);  // @property @type(color)
uniform vec3 u_RockColor = vec3(0.32f, 0.30f, 0.27f);   // @property @type(color)
uniform vec3 u_HighColor = vec3(0.55f, 0.47f, 0.34f);   // @property @type(color)
uniform float u_Roughness = 0.85f;                       // @property @range(0.05, 1.0)
uniform float u_RockSlope = 0.45f;                       // @property @range(0.0, 1.0)

void main()
{
	const vec3 N = TerrainNormal(vert2frag.WorldPosition.xz, vert2frag.Step);

	const float slope = clamp(1.0f - N.y, 0.0f, 1.0f);
	vec3 albedo = mix(u_GrassColor, u_RockColor, smoothstep(u_RockSlope * 0.5f, u_RockSlope, slope));

	const float height_t = clamp(vert2frag.WorldPosition.y / max(u_HeightScale, 1.0f), 0.0f, 1.0f);
	albedo = mix(albedo, u_HighColor, smoothstep(0.55f, 0.9f, height_t) * 0.65f);

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

	ObjectId = -1.0f;

	/* 调试分流：读数取自本笔实际着色用的量，与正常着色同源 */
	if (u_DebugView != 0)
	{
		SMaterialInput mtl = MakeDebugMaterialInput(albedo, N, u_Roughness, 0.0f);
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
