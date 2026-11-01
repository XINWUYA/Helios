/* 调试视图公共库：档位常量、通道选取、阴影累加编码。前向材质着色器与延迟调试着色器共用，
 * 序号跟 C++ DebugViewMode 一一对应（Kernel/src/Helios/Renderer/RenderCommon.h）。
 * 只放纯函数，不声明 uniform。注释里别写出 stage 声明关键字（ShaderCompiler 全文检索会误判）。 */

#ifndef DEBUG_VIEW_COMMON_GLSL
#define DEBUG_VIEW_COMMON_GLSL

#include "../Builtin/MaterialInput.glsl"

const int kDebugAlbedo = 1;
const int kDebugNormal = 2;
const int kDebugRoughness = 3;
const int kDebugMetallic = 4;
const int kDebugSpecularColor = 5;
const int kDebugAmbientOcclusion = 6;
const int kDebugEmission = 7;
const int kDebugAmbient = 8;
const int kDebugDiffuse = 9;
const int kDebugSpecular = 10;
const int kDebugShadow = 11;
const int kDebugIndirectDiffuse = 12;
const int kDebugIndirectSpecular = 13;
const int kDebugOverdraw = 14;
const int kDebugMipmap = 15;
const int kDebugProbe = 16;
const int kDebugWireframe = 18;

/* Overdraw 每层累加的计数，10 层饱和到 1.0 */
const float kOverdrawStep = 0.1f;

bool IsDebugSurfaceMode(int mode)
{
	return mode >= kDebugAlbedo && mode <= kDebugAmbient;
}

bool IsDebugIndirectMode(int mode)
{
	return mode == kDebugIndirectDiffuse || mode == kDebugIndirectSpecular;
}

/* 没有贴图与环境项的着色器用它凑一份材质输入，环境输入按 1 算（与平面环境项回退同口径） */
SMaterialInput MakeDebugMaterialInput(vec3 albedo, vec3 normal, float roughness, float metallic)
{
	SMaterialInput mtl;
	mtl.Albedo = albedo;
	mtl.Normal = normal;
	mtl.Roughness = roughness;
	mtl.Metallic = metallic;
	mtl.Specular = vec3(0.0f);
	mtl.Anisotropy = 0.0f;
	mtl.Emission = vec3(0.0f);
	mtl.Ambient = vec3(1.0f);
	mtl.AO = 1.0f;
	mtl.Opacity = 1.0f;
	mtl.IOR = 0.0f;
	mtl.ClearCoatThickness = 0.0f;
	mtl.ClearCoatRoughness = 0.0f;
	return mtl;
}

/* Surface 档的读数就是材质输入里对应的通道 */
vec3 DebugSurfaceChannel(int mode, SMaterialInput mtl)
{
	if (mode == kDebugAlbedo)
		return mtl.Albedo;
	if (mode == kDebugNormal)
		return normalize(mtl.Normal) * 0.5f + 0.5f;
	if (mode == kDebugRoughness)
		return vec3(mtl.Roughness);
	if (mode == kDebugMetallic)
		return vec3(mtl.Metallic);
	if (mode == kDebugSpecularColor)
		return mtl.Specular;
	if (mode == kDebugAmbientOcclusion)
		return vec3(mtl.AO);
	if (mode == kDebugEmission)
		return mtl.Emission;
	if (mode == kDebugAmbient)
		return mtl.Ambient;
	return vec3(0.0f);
}

/* 光照分量：直接光逐笔算好（已乘光源因子），环境项只在合成那一笔非零（已乘阴影可见度） */
struct SDebugLighting
{
	vec3 DirectDiffuse;
	vec3 DirectSpecular;
	vec3 IndirectDiffuse;
	vec3 IndirectSpecular;
};

vec3 DebugLightingChannel(int mode, SDebugLighting lit)
{
	if (mode == kDebugDiffuse)
		return lit.DirectDiffuse;
	if (mode == kDebugSpecular)
		return lit.DirectSpecular;
	if (mode == kDebugIndirectDiffuse)
		return lit.IndirectDiffuse;
	if (mode == kDebugIndirectSpecular)
		return lit.IndirectSpecular;
	return vec3(0.0f);
}

/* 阴影累加的一笔：r 可见度乘权重，g 权重，b 每笔加 2 当覆盖标记（背景停在清屏值，不超过 1）。
 * 权重只看灯够不够得着，不乘 N·L，否则法线沟槽会被踢出平均变成白点 */
vec4 DebugShadowSample(vec4 color_intensity, float attenuation, float shadow)
{
	const float luma = dot(color_intensity.rgb, vec3(0.2126f, 0.7152f, 0.0722f));
	const float weight = luma * color_intensity.a * attenuation;
	return vec4(weight * (1.0f - shadow), weight, 2.0f, 0.0f);
}

/* 探针序号的显示色：0 红、1 绿、2 蓝 */
vec3 DebugProbeColor(int index)
{
	if (index == 1)
		return vec3(0.1f, 1.0f, 0.1f);
	if (index == 2)
		return vec3(0.1f, 0.4f, 1.0f);
	return vec3(1.0f, 0.1f, 0.1f);
}

#endif // DEBUG_VIEW_COMMON_GLSL
