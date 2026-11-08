#type vertex
#version 410 core

/* 地形顶点：33×33 模板网格只带 UV，世界位置由 patch 信息推出来，
 * 高度在顶点着色器里采样高度图，顶点数据完全不落地。
 * 前向变体不写运动矢量，那一趟没有速度附件。 */
// @variant(TERRAIN_FORWARD, external)
// @variant(DEBUG_VIEW, external)

layout(location = 0) in vec2 a_Uv;
/* 实例数据：每 patch 一条 */
layout(location = 5) in vec4 a_PatchInfo; /* xyz = 左下角局部坐标，w = 边长 */
layout(location = 6) in vec4 a_PatchLod;  /* x = LOD 级数，y = 采样步长 */

#include "TerrainCommon.glsl"

struct SVextex2Frag
{
	vec3 WorldPosition;
	vec2 LocalXZ;
	float Step;
	float ReceiveShadow;
#if !TERRAIN_FORWARD
	vec4 CurClip;
	vec4 PrevClip;
#endif
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
#if !TERRAIN_FORWARD
	vert2frag.CurClip = u_VelocityViewProjectionMat * vec4(world, 1.0f);
	/* 地形是静态的：上一帧的同一个世界点用上一帧的视图投影，位移只来自相机 */
	vert2frag.PrevClip = u_PrevVelocityViewProjectionMat * vec4(world, 1.0f);
#endif
	vert2frag.WorldPosition = world;
	vert2frag.LocalXZ = local_xz;
	/* 法线固定按本 patch 的步长差分：跟着 morph 放粗会让坡度的混色沿边界切出色带 */
	vert2frag.Step = max(a_PatchLod.y, 0.05f);
	vert2frag.ReceiveShadow = u_TerrainReceiveShadow;
}


#type fragment
#version 410 core

#include "TerrainCommon.glsl"
#include "TerrainMaterial.glsl"
#include "TerrainVTSample.glsl"
#include "../Builtin/Math.glsl"

/* 两条管线要的东西不一样：前向自己算光照，延迟只填 G-Buffer。
 * 包在 #if 里是为了让各自的反射表干净，别带上一堆用不到的采样器 */
#if TERRAIN_FORWARD
#include "../Builtin/DirectLight.glsl"
#include "../Builtin/LightCompose.glsl"
#if DEBUG_VIEW
#include "../DebugShaders/DebugForward.glsl"
#endif
#else
#include "../Builtin/Velocity.glsl"
#endif

#if TERRAIN_FORWARD
layout(location = 0) out vec4 OutFragColor;
layout(location = 1) out float ObjectId;
#else
layout(location = 0) out vec4 GBufferTexture0; /* rgb: Albedo; a: Roughness */
layout(location = 1) out vec4 GBufferTexture1; /* rgb: WorldNormal; a: Metallic */
layout(location = 2) out vec4 GBufferTexture2; /* rgb: Specular; a: AO */
layout(location = 3) out vec4 GBufferTexture3; /* rgb: Emission; a: IOR */
layout(location = 4) out vec4 GBufferTexture4; /* rgb: Ambient; a: Anisotropy */
layout(location = 5) out vec4 GBufferTexture5; /* rgb: WorldPosition; a: 接受阴影 */
layout(location = 6) out float ObjectId;
layout(location = 7) out vec2 Velocity;
#endif

struct SVextex2Frag
{
	vec3 WorldPosition;
	vec2 LocalXZ;
	float Step;
	float ReceiveShadow;
#if !TERRAIN_FORWARD
	vec4 CurClip;
	vec4 PrevClip;
#endif
};

in SVextex2Frag vert2frag;

/* 地块实体的 id（per-draw） */
uniform int u_TerrainObjectId = -1;

void main()
{
	STerrainSurface surface = EvaluateTerrainSurface(vert2frag.LocalXZ);

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

#if TERRAIN_FORWARD
	const vec3 N = surface.Normal;
	const vec3 albedo = surface.Albedo;

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

	/* 调试分流：读数取自本笔实际着色用的量，与正常着色同源。走调试变体时才编进来 */
#if DEBUG_VIEW
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
#endif

	OutFragColor = vec4(direct + ambient, 1.0f);
#else
	GBufferTexture0 = vec4(surface.Albedo, surface.Roughness);
	/* 法线按 n * 0.5 + 0.5 编码，与 GBuffer.glsl 同一约定 */
	GBufferTexture1 = vec4(surface.Normal * 0.5f + 0.5f, 0.0f);
	GBufferTexture2 = vec4(vec3(0.04f), 1.0f);
	GBufferTexture3 = vec4(0.0f);
	GBufferTexture4 = vec4(0.0f);
	GBufferTexture5 = vec4(vert2frag.WorldPosition, vert2frag.ReceiveShadow);

	/* 地形可以按像素拾取：写实体 id，跟模型走同一套 */
	ObjectId = float(u_TerrainObjectId);
	Velocity = ComputeVelocityFromClip(vert2frag.CurClip, vert2frag.PrevClip);
#endif
}
