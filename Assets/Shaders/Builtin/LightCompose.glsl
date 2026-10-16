#ifndef LIGHT_COMPOSE_GLSL
#define LIGHT_COMPOSE_GLSL

#include "Uniforms.glsl"
#include "BRDF.glsl"

/* 光源因子：颜色强度 × 衰减 × 阴影衰减（直接光与调试拆分共用；0.8 = 阴影软衰减系数） */
vec3 LightFactor(float attenuation, float shadow)
{
	return u_ColorIntensity.rgb * u_ColorIntensity.a * attenuation * (1.0f - shadow * 0.8f);
}

/* 直接光合成（前向逐光源一笔 / 延迟全屏光照同口径） */
vec3 ComposeDirectLight(vec3 L, vec3 V, vec3 N, float metallic, float roughness, vec3 albedo,
	float attenuation, float shadow)
{
	return max(vec3(0.0f), BRDF(L, V, N, metallic, roughness, albedo)) * LightFactor(attenuation, shadow);
}

/* 探针 IBL 合成：漫反射 kd × 辐照度 × albedo + 镜面 预滤波 × 环境 BRDF。
 * 取图在外（前向单探针绑定 / 延迟 top2 混合各取各的），这里只做合成。 */
vec3 ComposeIBL(vec3 N, vec3 V, float metallic, vec3 albedo,
	vec3 irradiance, vec3 prefiltered, vec2 brdf)
{
	const float ndv = max(dot(N, V), 0.0f);
	vec3 F = F_Schlick(ndv, metallic, albedo);
	vec3 kd = (vec3(1.0f) - F) * (1.0f - metallic);
	vec3 specular = prefiltered * (F * brdf.x + brdf.y);
	return kd * irradiance * albedo + specular;
}

/* 无探针回退环境项：平面环境 × albedo × AO × 0.3（0.3 系数两条管线同口径） */
vec3 ComposeFallbackAmbient(vec3 ambient, vec3 albedo, float ao)
{
	return ambient * albedo * ao * 0.3f;
}

#endif // LIGHT_COMPOSE_GLSL
