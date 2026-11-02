#type vertex
#version 410 core

/* 地形的调试重绘：Overdraw / Mipmap / Wireframe 档。
 * 地形不进可见网格列表，这三档要单独补一笔才看得到；Surface 类在延迟管线里读 G-Buffer，
 * 前向管线由 TerrainForward.glsl 自己出读数，都不走这里。 */
layout(location = 0) in vec2 a_Uv;
layout(location = 5) in vec4 a_PatchInfo;
layout(location = 6) in vec4 a_PatchLod;

#include "../MaterialShaders/TerrainCommon.glsl"

struct SVextex2Frag
{
	vec3 WorldPosition;
	float Step;
};

out SVextex2Frag vert2frag;

void main()
{
	const vec3 world = TerrainVertexWorld(a_Uv, a_PatchInfo);
	gl_Position = u_ViewProjectionMat * vec4(world, 1.0f);
	vert2frag.WorldPosition = world;
	vert2frag.Step = max(a_PatchLod.y, 0.05f);
}


#type fragment
#version 410 core

#include "DebugViewCommon.glsl"
#include "../MaterialShaders/TerrainCommon.glsl"

struct SVextex2Frag
{
	vec3 WorldPosition;
	float Step;
};

in SVextex2Frag vert2frag;
layout(location = 0) out vec4 OutFragColor;

uniform int u_DebugMode = 0;
/* 线框色：与 Overlay/Wireframe.glsl 同名，材质侧写同一份取值 */
uniform vec4 u_WireframeColor = vec4(0.62f, 0.94f, 1.0f, 1.0f);

/* 外观参数与 TerrainForward.glsl 同名同值，per-draw 覆盖从源材质带过来 */
uniform vec3 u_GrassColor = vec3(0.26f, 0.36f, 0.16f);  // @property @type(color)
uniform vec3 u_RockColor = vec3(0.32f, 0.30f, 0.27f);   // @property @type(color)
uniform vec3 u_HighColor = vec3(0.55f, 0.47f, 0.34f);   // @property @type(color)
uniform float u_Roughness = 0.85f;                       // @property @range(0.05, 1.0)
uniform float u_RockSlope = 0.45f;                       // @property @range(0.0, 1.0)

void main()
{
	/* 重叠计数：加法混合下每个片元累加一份步进，显示层交给 DebugView.glsl 的热力图 */
	if (u_DebugMode == kDebugOverdraw)
	{
		OutFragColor = vec4(kOverdrawStep);
		return;
	}
	if (u_DebugMode == kDebugWireframe)
	{
		OutFragColor = vec4(u_WireframeColor.rgb, 1.0f);
		return;
	}

	const vec3 N = TerrainNormal(vert2frag.WorldPosition.xz, vert2frag.Step);
	const float slope = clamp(1.0f - N.y, 0.0f, 1.0f);
	vec3 albedo = mix(u_GrassColor, u_RockColor, smoothstep(u_RockSlope * 0.5f, u_RockSlope, slope));

	const float height_t = clamp(vert2frag.WorldPosition.y / max(u_HeightScale, 1.0f), 0.0f, 1.0f);
	albedo = mix(albedo, u_HighColor, smoothstep(0.55f, 0.9f, height_t) * 0.65f);

	/* 地形颜色是程序化算的，没有贴图也就没有 mip 层级可看，Mipmap 档按底色画 */
	SMaterialInput mtl = MakeDebugMaterialInput(albedo, N, u_Roughness, 0.0f);
	const int mode = u_DebugMode == kDebugMipmap ? kDebugAlbedo : u_DebugMode;
	OutFragColor = vec4(DebugSurfaceChannel(mode, mtl), 1.0f);
}
