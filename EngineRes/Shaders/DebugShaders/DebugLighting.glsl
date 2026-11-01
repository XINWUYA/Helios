/* 延迟光照的调试分流（Diffuse / Specular / Indirect Diffuse / Indirect Specular / Probe）：
 * include 片段（没有自己的 stage / main），由 Lighting.glsl 在函数定义之后调用；
 * 注释里别写出 stage 声明关键字（ShaderCompiler 全文检索会误判）。Shadow 档要叠加多盏灯，
 * 在 DebugShaders/DebugShadow.glsl 里单独累加。前向的同名分流见 DebugForward.glsl */

#ifndef DEBUG_LIGHTING_GLSL
#define DEBUG_LIGHTING_GLSL

#include "DebugViewCommon.glsl"

uniform int u_DebugView = 0;

/* 命中调试档位时把颜色写进 out_color 并返回 true（调用方负责输出）：
 * u_DebugView = 0（正常着色）返回 false；环境分量（Indirect 两档）只由第一笔光照合成 */
bool DebugLightingCompose(SGBufferData gbuffer, vec3 world_pos, vec3 n, vec3 v, vec3 l,
	float attenuation, float shadow, out vec3 out_color)
{
	out_color = vec3(0.0f);
	if (u_DebugView == 0)
		return false;

	if (u_DebugView == kDebugProbe)
	{
		/* 探针选择可视化：混合区显示两色渐变；未启用混合或全部出盒的回落（距离最近）压暗显示 */
		if (u_ProbeCount > 0)
		{
			const SProbeSelection selection = SelectIBLProbes(world_pos);
			vec3 color = DebugProbeColor(selection.Index0);
			if (selection.Weight1 > 0.0f)
				color = mix(color, DebugProbeColor(selection.Index1), selection.Weight1);
			out_color = selection.Weighted ? color : color * 0.35f;
		}
		return true;
	}

	if (u_DebugView != kDebugDiffuse && u_DebugView != kDebugSpecular && !IsDebugIndirectMode(u_DebugView))
		return false;

	/* 与正常着色同口径：直接光乘光源因子，环境项乘阴影可见度；平面环境项只有漫反射 */
	SDebugLighting lit;
	const vec3 light_factor = LightFactor(attenuation, shadow);
	lit.DirectDiffuse = max(vec3(0.0f), BRDFDiffuse(l, v, n, gbuffer.Metallic, gbuffer.Roughness, gbuffer.Albedo)) * light_factor;
	lit.DirectSpecular = max(vec3(0.0f), BRDFSpecular(l, v, n, gbuffer.Metallic, gbuffer.Roughness, gbuffer.Albedo)) * light_factor;
	lit.IndirectDiffuse = vec3(0.0f);
	lit.IndirectSpecular = vec3(0.0f);
	if (IsDebugIndirectMode(u_DebugView) && u_ComposeAmbientEmission > 0)
	{
		const float visibility = AmbientShadowVisibility(n, l, shadow);
		if (u_ProbeCount > 0)
		{
			vec3 ibl_diffuse;
			vec3 ibl_specular;
			EvaluateProbeIBLComponents(SelectIBLProbes(world_pos), n, v, gbuffer.Roughness,
				gbuffer.Metallic, gbuffer.Albedo, ibl_diffuse, ibl_specular);
			lit.IndirectDiffuse = max(vec3(0.0f), ibl_diffuse) * visibility;
			lit.IndirectSpecular = max(vec3(0.0f), ibl_specular) * visibility;
		}
		else
		{
			lit.IndirectDiffuse = max(vec3(0.0f), ComposeFallbackAmbient(gbuffer.Ambient, gbuffer.Albedo, gbuffer.AO))
				* visibility;
		}
	}

	out_color = DebugLightingChannel(u_DebugView, lit);
	return true;
}

#endif
