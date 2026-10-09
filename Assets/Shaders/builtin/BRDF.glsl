#ifndef BRDF_GLSL
#define BRDF_GLSL

#include "Math.glsl"

// Normal Distribution function --------------------------------------
float D_GGX(float dotNH, float roughness)
{
	float alpha = roughness * roughness;
	float alpha2 = alpha * alpha;
	float denom = dotNH * dotNH * (alpha2 - 1.0) + 1.0;
	return (alpha2)/(PI * denom*denom);
}

// Geometric Shadowing function --------------------------------------
float G_SchlicksmithGGX(float dotNL, float dotNV, float roughness)
{
	float r = (roughness + 1.0);
	float k = (r*r) / 8.0;
	float GL = dotNL / (dotNL * (1.0 - k) + k);
	float GV = dotNV / (dotNV * (1.0 - k) + k);
	return GL * GV;
}

// Fresnel function ----------------------------------------------------
vec3 F_Schlick(float cosTheta, float metallic, vec3 albedo)
{
	vec3 F0 = mix(vec3(0.04), albedo, metallic);
	vec3 F = F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
	return F;
}

// Diffuse And Specular BRDF composition --------------------------------------------
vec3 BRDF(vec3 L, vec3 V, vec3 N, float metallic, float roughness, vec3 albedo)
{
	// Precalculate vectors and dot products	
	vec3 H = normalize (V + L);
	float dotNV = clamp(dot(N, V), 0.0, 1.0);
	float dotNL = clamp(dot(N, L), 0.0, 1.0);
	float dotLH = clamp(dot(L, H), 0.0, 1.0);
	float dotNH = clamp(dot(N, H), 0.0, 1.0);

	vec3 color = vec3(0.0);

	if (dotNL > 0.0)
	{
		float rroughness = max(0.05, roughness);
		// D = Normal distribution (Distribution of the microfacets)
		float D = D_GGX(dotNH, roughness);
		// G = Geometric shadowing term (Microfacets shadowing)
		float G = G_SchlicksmithGGX(dotNL, dotNV, rroughness);
		// F = Fresnel factor (Reflectance depending on angle of incidence)
		vec3 F = F_Schlick(dotNV, metallic, albedo);
		
		vec3 kD = vec3(1.0) - F;
		vec3 diff = kD * (1.0 - metallic) * albedo / PI;
		vec3 spec = D * F * G / (4.0 * dotNL * dotNV);

		color += (diff + spec) * dotNL;
	}

	return color;
}

// 漫反射 / 高光分量拆分（调试视图的 Diffuse / Specular 档）——
// 口径与上方合成一致：两者相加即 BRDF 的返回值
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