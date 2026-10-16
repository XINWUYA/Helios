#type vertex
#version 410 core

layout(location = 0) in vec2 a_Position;
layout(location = 0) out vec2 v2f_TexCoord;

void main()
{
	v2f_TexCoord = a_Position * 0.5f + 0.5f;
	gl_Position = vec4(a_Position, 0.0f, 1.0f);
}


#type fragment
#version 410 core

#include "../Builtin/Math.glsl"
#include "BakeCommon.glsl"

layout(location = 0) in vec2 v2f_TexCoord;
layout(location = 0) out vec4 OutFragColor;

/* 环境 BRDF 查找表（Karis split-sum 的第一部分）：按 (NdotV, roughness) 积分出
 * 菲涅尔缩放系数 A/B，运行期以 textureLod(...).rg 查表。烘焙一次即可，不参与实时帧 */
float DistributionGGX(float n_dot_h, float roughness)
{
	float a = roughness * roughness;
	float a2 = a * a;
	float denom = n_dot_h * n_dot_h * (a2 - 1.0f) + 1.0f;
	return a2 / (PI * denom * denom);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
	float a = roughness;
	float k = (a * a) / 2.0;

	float nom   = NdotV;
	float denom = NdotV * (1.0 - k) + k;

	return nom / denom;
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
	float NdotV = max(dot(N, V), 0.0);
	float NdotL = max(dot(N, L), 0.0);
	float ggx2 = GeometrySchlickGGX(NdotV, roughness);
	float ggx1 = GeometrySchlickGGX(NdotL, roughness);

	return ggx1 * ggx2;
}

vec2 IntegrateBRDF(float n_dot_v, float roughness)
{
	vec3 v = vec3(sqrt(1.0f - n_dot_v * n_dot_v), 0.0f, n_dot_v);
	vec3 n = vec3(0.0f, 0.0f, 1.0f);

	float a = 0.0f;
	float b = 0.0f;

	const uint k_SampleCount = 1024u;
	for (uint i = 0u; i < k_SampleCount; ++i)
	{
		vec2 xi = Hammersley(i, k_SampleCount);
		vec3 h  = ImportanceSampleGGX(xi, n, roughness);
		vec3 l  = normalize(2.0f * dot(v, h) * h - v);

		float n_dot_l = max(l.z, 0.0f);
		float n_dot_h = max(h.z, 0.0f);
		float v_dot_h = max(dot(v, h), 0.0f);

		if (n_dot_l > 0.0f)
		{
			float g = GeometrySmith(n, v, l, roughness);
			float g_vis = (g * v_dot_h) / (n_dot_h * n_dot_v);

			float fresnel = pow(1.0f - v_dot_h, 5.0f);
			a += (1.0f - fresnel) * g_vis;
			b += fresnel * g_vis;
		}
	}

	return vec2(a, b) / float(k_SampleCount);
}

void main()
{
	vec2 result = IntegrateBRDF(v2f_TexCoord.x, v2f_TexCoord.y);
	OutFragColor = vec4(result, 0.0f, 1.0f);
}
