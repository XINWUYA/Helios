#type vertex
#version 410 core

layout(location = 0) in vec3 a_Position;
out vec3 WorldPosition;

void main()
{
	WorldPosition = a_Position;
	gl_Position = vec4(a_Position, 1.0f);
}


#type fragment
#version 410 core

#include "../Builtin/Math.glsl"
#include "BakeCommon.glsl"

in vec3 WorldPosition;
layout(location = 0) out vec4 OutFragColor;

uniform samplerCube u_EnvironmentMap;
uniform int u_FaceId;
/* 源环境立方图的最大 mip 层级（= log2(面尺寸)），用于换算源 texel 的立体角 */
uniform float u_EnvironmentMapMaxMip;

void main()
{
	vec3 normal = GetCubeFaceDirection(u_FaceId, WorldPosition.xy);

	/* 以法线为 z 轴构造切线基 */
	vec3 up = abs(normal.z) < 0.999f ? vec3(0.0f, 0.0f, 1.0f) : vec3(1.0f, 0.0f, 0.0f);
	vec3 tangent = normalize(cross(up, normal));
	vec3 bitangent = cross(normal, tangent);

	vec3 irradiance = vec3(0.0f);

	/* 源立方图单个 texel 的立体角（6 个面共 6*size^2 个 texel） */
	float env_size = exp2(u_EnvironmentMapMaxMip);
	float sa_texel = 4.0f * PI / (6.0f * env_size * env_size);

	const float k_SampleDelta = 0.025f;
	float sample_count = 0.0f;
	for (float phi = 0.0f; phi < 2.0f * PI; phi += k_SampleDelta)
	{
		for (float theta = 0.0f; theta < 0.5f * PI; theta += k_SampleDelta)
		{
			/* 球坐标 → 切空间 → 世界空间 */
			vec3 tangent_sample = vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
			vec3 sample_vec = tangent_sample.x * tangent + tangent_sample.y * bitangent + tangent_sample.z * normal;

			/* 按本次采样的立体角（≈Δθ·Δφ·sinθ）选源 mip：避免太阳这类极亮 texel
			 * 被点采样整颗命中，在辐照度里留下白斑 */
			float sa_sample = k_SampleDelta * k_SampleDelta * sin(theta);
			float source_mip = 0.5f * log2(max(sa_sample / sa_texel, 1.0f));

			irradiance += textureLod(u_EnvironmentMap, sample_vec, source_mip).rgb * cos(theta) * sin(theta);
			++sample_count;
		}
	}

	irradiance = PI * irradiance * (1.0f / sample_count);
	OutFragColor = vec4(irradiance, 1.0f);
}
