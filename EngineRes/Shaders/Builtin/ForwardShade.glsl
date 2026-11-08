#ifndef FORWARD_SHADE_GLSL
#define FORWARD_SHADE_GLSL

#include "Math.glsl"
#include "Uniforms.glsl"
#include "BRDF.glsl"
#include "MaterialInput.glsl"
#include "ShadowUtils.glsl"
#include "DirectLight.glsl"
#include "LightCompose.glsl"
#include "../ReflectionProbe/IBLCommon.glsl"
#if DEBUG_VIEW
#include "../DebugShaders/DebugForward.glsl"
#endif

/* 前向着色结果。调试读数的 alpha 是累加占位（非合成笔返回 0），不能替换成 1，
 * 所以整份带出来，由调用方决定怎么用 */
struct SForwardShade
{
	vec4 Color;
	bool DebugReadout;
};

/* 前向材质的着色流程：直接光 + 间接光 + 自发光，含调试分流。
 * 材质差别都在 SMaterialInput 里，改光照公式只有这一份。 */
SForwardShade ShadeForwardSurface(const SMaterialInput mtl, vec3 world_position)
{
	SForwardShade shade;
	const vec3 N = mtl.Normal;
	const vec3 V = normalize(u_ViewPos - world_position);

	vec3 L;
	float attenuation;
	float shadow;
	EvaluateDirectLight(u_ShadowMap, world_position, N, L, attenuation, shadow);
	shadow *= u_ReceiveShadow;

	/* 调试视图：走调试变体时才编进来，命中就直接给读数 */
#if DEBUG_VIEW
	if (u_DebugView != 0)
	{
		shade.Color = DebugForwardOutput(mtl, DebugStandardLighting(mtl, N, V, L, attenuation, shadow),
			attenuation, shadow);
		shade.DebugReadout = true;
		return shade;
	}
#endif

	vec3 color = ComposeDirectLight(L, V, N, mtl.Metallic, mtl.Roughness, mtl.Albedo,
		attenuation, shadow);

	/* 环境项 + 自发光只在合成的那一笔里累加（多光源时其余笔只叠直接光） */
	if (u_ComposeAmbientEmission > 0)
	{
		vec3 ambient;
		if (u_UseIBL != 0)
			ambient = max(vec3(0.0f), EvaluateIBL(N, V, mtl.Roughness, mtl.Metallic, mtl.Albedo));
		else
			ambient = max(vec3(0.0f), ComposeFallbackAmbient(mtl.Ambient, mtl.Albedo, mtl.AO));
		color += ambient * AmbientShadowVisibility(N, L, shadow) + mtl.Emission;
	}

	/* alpha 留给调用方填不透明度 */
	shade.Color = vec4(color, 0.0f);
	shade.DebugReadout = false;
	return shade;
}

#endif // FORWARD_SHADE_GLSL
