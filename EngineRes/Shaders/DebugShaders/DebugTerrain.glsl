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
	float morph = 1.0f;
	const vec3 world = TerrainVertexWorld(a_Uv, a_PatchInfo, a_PatchLod, morph);
	gl_Position = u_ViewProjectionMat * vec4(world, 1.0f);
	vert2frag.WorldPosition = world;

	vert2frag.Step = max(a_PatchLod.y, 0.05f);
}


#type fragment
#version 410 core

#include "DebugViewCommon.glsl"
#include "../MaterialShaders/TerrainCommon.glsl"
#include "../MaterialShaders/TerrainMaterial.glsl"
#include "../MaterialShaders/TerrainVTSample.glsl"

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

	STerrainSurface surface = EvaluateTerrainSurface(vert2frag.WorldPosition.xz);

	/* 调试读数要跟正常着色同源：页驻留时取页池里的颜色 */
	vec4 page_albedo;
	if (TerrainVirtualTextureLookup(vert2frag.WorldPosition.xz, page_albedo))
	{
		surface.Albedo = page_albedo.rgb;
		surface.Roughness = page_albedo.a;
	}

	/* 地形颜色是程序化算的，没有贴图也就没有 mip 层级可看，Mipmap 档按底色画 */
	SMaterialInput mtl = MakeDebugMaterialInput(surface.Albedo, surface.Normal, surface.Roughness, 0.0f);
	const int mode = u_DebugMode == kDebugMipmap ? kDebugAlbedo : u_DebugMode;
	OutFragColor = vec4(DebugSurfaceChannel(mode, mtl), 1.0f);
}
