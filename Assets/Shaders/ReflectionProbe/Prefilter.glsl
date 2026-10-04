#type vertex
#version 410 core

#include "../builtin/Uniforms.glsl"

layout(location = 0) in vec3 a_Position;
layout(location = 0) out vec3 WorldPosition;

void main()
{
	WorldPosition = a_Position;
	gl_Position = vec4(a_Position, 1.0f);
}


#type fragment
#version 410 core

#include "../builtin/Math.glsl"
#include "IBLCommon.glsl"

layout(location = 0) in vec3 WorldPosition;
layout(location = 0) out vec4 OutColor;

layout(binding = 0) uniform samplerCube u_EnvironmentMap;
layout(location = 0) uniform int u_FaceId;
layout(location = 1) uniform float u_Roughness;
layout(location = 2) uniform float u_EnvironmentMapMaxMip;

float DistributionGGX(float n_dot_h, float roughness)
{
	float a = roughness * roughness;
	float a2 = a * a;
	float denom = n_dot_h * n_dot_h * (a2 - 1.0f) + 1.0f;
	return a2 / (PI * denom * denom);
}

void main()
{
	vec3 n = GetCubeFaceDirection(u_FaceId, WorldPosition.xy);
	vec3 v = n;

	const uint k_SampleCount = 1024u;
	float total_weight = 0.0f;

	/* 源立方图单个 texel 的立体角（6 个面共 6*size^2 个 texel） */
	float env_size = exp2(u_EnvironmentMapMaxMip);
	float sa_texel = 4.0f * PI / (6.0f * env_size * env_size);

	vec3 prefiltered_color = vec3(0.0f);
	for (uint i = 0u; i < k_SampleCount; ++i)
	{
		vec2 xi = Hammersley(i, k_SampleCount);
        vec3 h  = ImportanceSampleGGX(xi, n, u_Roughness);
        float v_dot_h = max(dot(v, h), 0.0f);
        vec3 l  = normalize(2.0f * v_dot_h * h - v);

		float n_dot_l = max(dot(n, l), 0.0f);
		float n_dot_h = max(dot(n, h), 0.0f);
		if (n_dot_l > 0.0f)
		{
			/* 按采样立体角选择源 mip（Karis 的 split-sum 预滤波做法）：
			 * 采样覆盖的立体角越小越该读清晰层级；固定读 mip0 会让太阳这类
			 * 极亮 texel 主导结果，表现为孤立的白色方块（走样）。 */
			float pdf = DistributionGGX(n_dot_h, u_Roughness) * n_dot_h / (4.0f * v_dot_h) + 0.0001f;
			float sa_sample = 1.0f / (float(k_SampleCount) * pdf + 0.0001f);
			float source_mip = (u_Roughness == 0.0f) ? 0.0f : 0.5f * log2(max(sa_sample / sa_texel, 1.0f));

			prefiltered_color += textureLod(u_EnvironmentMap, l, source_mip).rgb * n_dot_l;
			total_weight += n_dot_l;
		}
	}

	OutColor = vec4(prefiltered_color / max(total_weight, 0.0001f), 1.0f);
}
