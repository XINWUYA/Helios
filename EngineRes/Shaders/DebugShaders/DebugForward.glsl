/* 前向调试分流：前向材质着色器算完材质输入与本笔光照之后调用，命中档位直接输出调试读数。
 * 档位由前向场景 Pass 逐绘制下发，0 是正常着色；读数取自着色器自己的输入，与正常着色同源。
 * include 片段，注释里别写出 stage 声明关键字（ShaderCompiler 全文检索会误判）。 */

#ifndef DEBUG_FORWARD_GLSL
#define DEBUG_FORWARD_GLSL

#include "../Builtin/Uniforms.glsl"
#include "../Builtin/DirectLight.glsl"
#include "../Builtin/LightCompose.glsl"
#include "../ReflectionProbe/IBLCommon.glsl"
#include "DebugViewCommon.glsl"

/* 声明它就算接入了调试分流（Material::SupportsDebugView 按它判断） */
uniform int u_DebugView = 0;
/* Probe 档：本物体选中的探针在视图探针表里的序号，-1 = 不在表里 */
uniform int u_DebugProbeIndex = -1;

/* 标准 BRDF 着色器的光照分量，与 ComposeDirectLight 和环境项合成同口径 */
SDebugLighting DebugStandardLighting(SMaterialInput mtl, vec3 n, vec3 v, vec3 l, float attenuation, float shadow)
{
	SDebugLighting lit;
	const vec3 light_factor = LightFactor(attenuation, shadow);
	lit.DirectDiffuse = max(vec3(0.0f), BRDFDiffuse(l, v, n, mtl.Metallic, mtl.Roughness, mtl.Albedo)) * light_factor;
	lit.DirectSpecular = max(vec3(0.0f), BRDFSpecular(l, v, n, mtl.Metallic, mtl.Roughness, mtl.Albedo)) * light_factor;
	lit.IndirectDiffuse = vec3(0.0f);
	lit.IndirectSpecular = vec3(0.0f);
	if (IsDebugIndirectMode(u_DebugView) && u_ComposeAmbientEmission > 0)
	{
		const float visibility = AmbientShadowVisibility(n, l, shadow);
		if (u_UseIBL != 0)
		{
			vec3 ibl_diffuse;
			vec3 ibl_specular;
			EvaluateIBLComponents(n, v, mtl.Roughness, mtl.Metallic, mtl.Albedo, ibl_diffuse, ibl_specular);
			lit.IndirectDiffuse = max(vec3(0.0f), ibl_diffuse) * visibility;
			lit.IndirectSpecular = max(vec3(0.0f), ibl_specular) * visibility;
		}
		else
		{
			lit.IndirectDiffuse = max(vec3(0.0f), ComposeFallbackAmbient(mtl.Ambient, mtl.Albedo, mtl.AO)) * visibility;
		}
	}
	return lit;
}

/* 调试读数（u_DebugView 非 0 时调用）。只出一次的读数放在合成那一笔，其余笔输出 0 叠加；
 * Shadow 档输出累加编码，由调试合成 Pass 相除 */
vec4 DebugForwardOutput(SMaterialInput mtl, SDebugLighting lit, float attenuation, float shadow)
{
	if (u_DebugView == kDebugShadow)
		return DebugShadowSample(u_ColorIntensity, attenuation, shadow);

	if (u_ComposeAmbientEmission == 0 && u_DebugView != kDebugDiffuse && u_DebugView != kDebugSpecular)
		return vec4(0.0f);

	if (IsDebugSurfaceMode(u_DebugView))
		return vec4(DebugSurfaceChannel(u_DebugView, mtl), 1.0f);

	if (u_DebugView == kDebugProbe)
	{
		/* 前向按物体挑最近的一个探针、不混合，与延迟的"距离最近"路径一样压暗显示 */
		if (u_UseIBL == 0)
			return vec4(0.0f, 0.0f, 0.0f, 1.0f);
		const vec3 color = u_DebugProbeIndex >= 0 ? DebugProbeColor(u_DebugProbeIndex) : vec3(1.0f);
		return vec4(color * 0.35f, 1.0f);
	}

	return vec4(DebugLightingChannel(u_DebugView, lit), 1.0f);
}

#endif // DEBUG_FORWARD_GLSL
