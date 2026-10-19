#ifndef LIGHT_COMPOSE_GLSL
#define LIGHT_COMPOSE_GLSL

#include "Uniforms.glsl"
#include "BRDF.glsl"

/* 光源因子：颜色强度 × 衰减 × 阴影衰减（直接光与调试拆分共用；0.8 = 阴影软衰减系数） */
vec3 LightFactor(float attenuation, float shadow)
{
	return u_ColorIntensity.rgb * u_ColorIntensity.a * attenuation * (1.0f - shadow * 0.8f);
}

/* 环境项的阴影可见度：引擎没有 GI，阴影图只压直接光，环境光（IBL / 平面环境项）会把
 * 小物体的影子冲淡。这里按 N·L 压一压受光面的环境项 —— 背光面 N·L = 0 不动，
 * 它们的暗来自没有直接光，不是被挡住。 */
float AmbientShadowVisibility(vec3 N, vec3 L, float shadow)
{
	return 1.0f - shadow * clamp(dot(N, L), 0.0f, 1.0f);
}

/* 直接光合成（前向逐光源一笔 / 延迟全屏光照同口径） */
vec3 ComposeDirectLight(vec3 L, vec3 V, vec3 N, float metallic, float roughness, vec3 albedo,
	float attenuation, float shadow)
{
	return max(vec3(0.0f), BRDF(L, V, N, metallic, roughness, albedo)) * LightFactor(attenuation, shadow);
}

/* IBL 漫反射分量：kd × 辐照度 × albedo */
vec3 ComposeIBLDiffuse(vec3 N, vec3 V, float metallic, vec3 albedo, vec3 irradiance)
{
	const float ndv = max(dot(N, V), 0.0f);
	const vec3 F = F_Schlick(ndv, metallic, albedo);
	const vec3 kd = (vec3(1.0f) - F) * (1.0f - metallic);
	return kd * irradiance * albedo;
}

/* IBL 镜面分量：预滤波 × 环境 BRDF */
vec3 ComposeIBLSpecular(vec3 N, vec3 V, float metallic, vec3 albedo, vec3 prefiltered, vec2 brdf)
{
	const float ndv = max(dot(N, V), 0.0f);
	const vec3 F = F_Schlick(ndv, metallic, albedo);
	return prefiltered * (F * brdf.x + brdf.y);
}

/* 探针 IBL 合成：漫反射 + 镜面。取图在外（前向单探针绑定 / 延迟 top2 混合各取各的），
 * 这里只做合成；调试视图按分量取值，正常着色取这份和。 */
vec3 ComposeIBL(vec3 N, vec3 V, float metallic, vec3 albedo,
	vec3 irradiance, vec3 prefiltered, vec2 brdf)
{
	return ComposeIBLDiffuse(N, V, metallic, albedo, irradiance)
		+ ComposeIBLSpecular(N, V, metallic, albedo, prefiltered, brdf);
}

/* 无探针回退环境项：平面环境 × albedo × AO × 0.3（0.3 系数两条管线同口径） */
vec3 ComposeFallbackAmbient(vec3 ambient, vec3 albedo, float ao)
{
	return ambient * albedo * ao * 0.3f;
}

#endif // LIGHT_COMPOSE_GLSL
