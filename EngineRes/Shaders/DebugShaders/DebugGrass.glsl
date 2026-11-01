#type vertex
#version 410 core

/* 草的调试重绘：Surface / Mipmap / Overdraw / Wireframe 档。
 * 草不进 G-Buffer、也不在可见网格列表里，调试 Pass 要单独补这一笔才看得到。 */
#include "DebugGrassVertex.glsl"


#type fragment
#version 410 core

#include "DebugViewCommon.glsl"

struct SVextex2Frag
{
	vec3 WorldPosition;
	vec3 WorldNormal;
	float Height;
	float ColorSeed;
	float ReceiveShadow;
};

in SVextex2Frag vert2frag;
layout(location = 0) out vec4 OutFragColor;

/* Shadow 档由 DebugGrassShadow.glsl 承担（要采阴影图） */
uniform int u_DebugMode = 0;

/* 底色与粗糙度跟源材质同名同值（逐片草地的 per-draw 覆盖带过来），读数才和正常着色一致 */
uniform vec3 u_BaseColor = vec3(0.09f, 0.20f, 0.05f);   // @property @type(color)
uniform vec3 u_TipColor = vec3(0.45f, 0.62f, 0.18f);    // @property @type(color)
uniform float u_Roughness = 0.75f;                      // @property
uniform int u_FlatPad = 0;                              // @property
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

	/* 与 Grass.glsl 同一套底色：根暗梢亮，块底面整体压暗一档 */
	vec3 albedo = mix(u_BaseColor, u_TipColor, vert2frag.Height) * (0.82f + 0.36f * vert2frag.ColorSeed);
	float ao = mix(0.35f, 1.0f, vert2frag.Height);
	if (u_FlatPad != 0)
	{
		albedo = mix(u_BaseColor, u_TipColor, 0.35f) * (0.75f + 0.25f * vert2frag.ColorSeed);
		ao = 1.0f;
	}

	/* 草的颜色是程序化算出来的，没有贴图也就没有 mip 层级可看，Mipmap 档按底色画；
	 * AO 读的是着色时用的根部自遮挡系数 */
	SMaterialInput mtl = MakeDebugMaterialInput(albedo, vert2frag.WorldNormal, u_Roughness, 0.0f);
	mtl.AO = ao;
	const int mode = u_DebugMode == kDebugMipmap ? kDebugAlbedo : u_DebugMode;
	OutFragColor = vec4(DebugSurfaceChannel(mode, mtl), 1.0f);
}
