#ifndef BRDF_GLSL
#define BRDF_GLSL

#include "Math.glsl"

/* 法线分布（GGX） */
float D_GGX(float dotNH, float roughness)
{
	float alpha = roughness * roughness;
	float alpha2 = alpha * alpha;
	float denom = dotNH * dotNH * (alpha2 - 1.0) + 1.0;
	return (alpha2) / (PI * denom * denom);
}

/* 几何遮蔽（Schlick-GGX） */
float G_SchlicksmithGGX(float dotNL, float dotNV, float roughness)
{
	float r = (roughness + 1.0);
	float k = (r * r) / 8.0;
	float GL = dotNL / (dotNL * (1.0 - k) + k);
	float GV = dotNV / (dotNV * (1.0 - k) + k);
	return GL * GV;
}

/* 菲涅尔（Schlick 近似）：pow(x, 5) 展开成乘法 */
vec3 F_Schlick(float cosTheta, float metallic, vec3 albedo)
{
	vec3 F0 = mix(vec3(0.04), albedo, metallic);
	float f = 1.0 - cosTheta;
	float f2 = f * f;
	vec3 F = F0 + (1.0 - F0) * (f2 * f2 * f);
	return F;
}

/* 完整 BRDF：漫反射 + 镜面 */
vec3 BRDF(vec3 L, vec3 V, vec3 N, float metallic, float roughness, vec3 albedo)
{
	vec3 H = normalize(V + L);
	float dotNV = clamp(dot(N, V), 0.0, 1.0);
	float dotNL = clamp(dot(N, L), 0.0, 1.0);
	float dotNH = clamp(dot(N, H), 0.0, 1.0);

	vec3 color = vec3(0.0);

	if (dotNL > 0.0)
	{
		float rroughness = max(0.05, roughness);
		float D = D_GGX(dotNH, roughness);
		float G = G_SchlicksmithGGX(dotNL, dotNV, rroughness);
		vec3 F = F_Schlick(dotNV, metallic, albedo);

		vec3 kD = vec3(1.0) - F;
		vec3 diff = kD * (1.0 - metallic) * albedo / PI;
		vec3 spec = D * F * G / (4.0 * dotNL * dotNV);

		color += (diff + spec) * dotNL;
	}

	return color;
}

/* 漫反射 / 高光分量拆分（调试视图的 Diffuse / Specular 档）：两者相加即 BRDF 的返回值 */
vec3 BRDFDiffuse(vec3 L, vec3 V, vec3 N, float metallic, float roughness, vec3 albedo)
{
	float dotNL = clamp(dot(N, L), 0.0, 1.0);
	float dotNV = clamp(dot(N, V), 0.0, 1.0);

	if (dotNL <= 0.0)
		return vec3(0.0);

	vec3 F = F_Schlick(dotNV, metallic, albedo);
	vec3 kD = vec3(1.0) - F;
	return kD * (1.0 - metallic) * albedo / PI * dotNL;
}

vec3 BRDFSpecular(vec3 L, vec3 V, vec3 N, float metallic, float roughness, vec3 albedo)
{
	vec3 H = normalize(V + L);
	float dotNL = clamp(dot(N, L), 0.0, 1.0);
	float dotNV = clamp(dot(N, V), 0.0, 1.0);
	float dotNH = clamp(dot(N, H), 0.0, 1.0);

	if (dotNL <= 0.0)
		return vec3(0.0);

	float rroughness = max(0.05, roughness);
	float D = D_GGX(dotNH, roughness);
	float G = G_SchlicksmithGGX(dotNL, dotNV, rroughness);
	vec3 F = F_Schlick(dotNV, metallic, albedo);
	return D * F * G / (4.0 * dotNL * dotNV) * dotNL;
}

#endif
