#type vertex
#version 410 core

/* 地形顶点：33×33 模板网格只带 UV，世界位置由 patch 信息推出来，
 * 高度在顶点着色器里采样高度图 —— 顶点数据完全不落地。 */
layout(location = 0) in vec2 a_Uv;
/* 实例数据：每 patch 一条 */
layout(location = 5) in vec4 a_PatchInfo; /* xyz = 左下角世界坐标，w = 边长 */
layout(location = 6) in vec4 a_PatchLod;  /* x = LOD 级数，y = 采样步长 */

#include "TerrainCommon.glsl"
#include "../Builtin/Velocity.glsl"

struct SVextex2Frag
{
	vec3 WorldPosition;
	float Step;
	float ReceiveShadow;
	vec4 CurClip;
	vec4 PrevClip;
};

out SVextex2Frag vert2frag;

uniform float u_TerrainReceiveShadow = 1.0f;

void main()
{
	float morph = 1.0f;
	const vec3 world = TerrainVertexWorld(a_Uv, a_PatchInfo, a_PatchLod.y, morph);

	gl_Position = u_ViewProjectionMat * vec4(world, 1.0f);
	vert2frag.CurClip = u_VelocityViewProjectionMat * vec4(world, 1.0f);
	/* 地形是静态的：上一帧的同一个世界点用上一帧的视图投影，位移只来自相机 */
	vert2frag.PrevClip = u_PrevVelocityViewProjectionMat * vec4(world, 1.0f);
	vert2frag.WorldPosition = world;
	/* 法线跟着几何走：边缘用粗一档的差分，接缝两侧的明暗才对得上 */
	const float step = max(a_PatchLod.y, 0.05f);
	vert2frag.Step = mix(step * 2.0f, step, morph);
	vert2frag.ReceiveShadow = u_TerrainReceiveShadow;
}


#type fragment
#version 410 core

#include "TerrainCommon.glsl"
#include "../Builtin/Math.glsl"
#include "../Builtin/Velocity.glsl"

layout(location = 0) out vec4 GBufferTexture0; /* rgb: Albedo; a: Roughness */
layout(location = 1) out vec4 GBufferTexture1; /* rgb: WorldNormal; a: Metallic */
layout(location = 2) out vec4 GBufferTexture2; /* rgb: Specular; a: AO */
layout(location = 3) out vec4 GBufferTexture3; /* rgb: Emission; a: IOR */
layout(location = 4) out vec4 GBufferTexture4; /* rgb: Ambient; a: Anisotropy */
layout(location = 5) out vec4 GBufferTexture5; /* rgb: WorldPosition; a: 接受阴影 */
layout(location = 6) out float ObjectId;
layout(location = 7) out vec2 Velocity;

struct SVextex2Frag
{
	vec3 WorldPosition;
	float Step;
	float ReceiveShadow;
	vec4 CurClip;
	vec4 PrevClip;
};

in SVextex2Frag vert2frag;

/* 外观按高度与坡度混合：低处草色、陡坡岩色、高处砂色。
 * 三层够用，真要做多层 splat 得等虚拟纹理（见方案文档 4.6）。 */
uniform vec3 u_GrassColor = vec3(0.26f, 0.36f, 0.16f);  // @property @type(color)
uniform vec3 u_RockColor = vec3(0.32f, 0.30f, 0.27f);   // @property @type(color)
uniform vec3 u_HighColor = vec3(0.55f, 0.47f, 0.34f);   // @property @type(color)
uniform float u_Roughness = 0.85f;                       // @property @range(0.05, 1.0)
uniform float u_RockSlope = 0.45f;                       // @property @range(0.0, 1.0)

void main()
{
	const vec3 N = TerrainNormal(vert2frag.WorldPosition.xz, vert2frag.Step);

	/* 坡度 = 1 - N.y：陡的地方露岩，缓的地方长草 */
	const float slope = clamp(1.0f - N.y, 0.0f, 1.0f);
	vec3 albedo = mix(u_GrassColor, u_RockColor, smoothstep(u_RockSlope * 0.5f, u_RockSlope, slope));

	/* 高处混一点砂色，远山才有层次 */
	const float height_t = clamp(vert2frag.WorldPosition.y / max(u_HeightScale, 1.0f), 0.0f, 1.0f);
	albedo = mix(albedo, u_HighColor, smoothstep(0.55f, 0.9f, height_t) * 0.65f);

	GBufferTexture0 = vec4(albedo, u_Roughness);
	/* 法线按 n * 0.5 + 0.5 编码，与 GBuffer.glsl 同一约定 */
	GBufferTexture1 = vec4(N * 0.5f + 0.5f, 0.0f);
	GBufferTexture2 = vec4(vec3(0.04f), 1.0f);
	GBufferTexture3 = vec4(0.0f);
	GBufferTexture4 = vec4(0.0f);
	GBufferTexture5 = vec4(vert2frag.WorldPosition, vert2frag.ReceiveShadow);

	/* 地形不参与拾取：与空像素同口径 */
	ObjectId = -1.0f;
	Velocity = ComputeVelocityFromClip(vert2frag.CurClip, vert2frag.PrevClip);
}
