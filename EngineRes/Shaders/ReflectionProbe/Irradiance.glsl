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
/* 源环境立方图的最大 mip 层级（= log2(面尺寸)），用于换算源 texel 的角尺寸 */
uniform float u_EnvironmentMapMaxMip;

void main()
{
	vec3 normal = GetCubeFaceDirection(u_FaceId, WorldPosition.xy);

	/* 整球按 0.1 弧度的经纬格点剖分，每格贡献 ΔθΔφ·sinθ·cosθ 的权重 */
	const float k_SampleDelta = 0.1f;

	vec3 irradiance = vec3(0.0f);

	/* 源 mip 按格点间距选：一次采样要糊住一个格点，太阳这种零点几度却亮出天空
	 * 两个数量级的特征才不会在格点之间忽进忽出 */
	float env_size = exp2(u_EnvironmentMapMaxMip);
	float texel_angle = (0.5f * PI) / env_size;
	float source_mip = log2(max(k_SampleDelta / texel_angle, 1.0f));

	/* 格点固定在贴图空间，不跟着法线转，法线只进余弦权重。按法线摆一套格点的话，
	 * 同一个源 texel 在相邻像素上会跳进跳出，条纹就是这么来的 */
	for (float theta = 0.5f * k_SampleDelta; theta < PI; theta += k_SampleDelta)
	{
		float sin_theta = sin(theta);
		for (float phi = 0.5f * k_SampleDelta; phi < 2.0f * PI; phi += k_SampleDelta)
		{
			vec3 direction = vec3(sin_theta * cos(phi), cos(theta), sin_theta * sin(phi));
			float ndl = dot(normal, direction);
			if (ndl <= 0.0f)
				continue;

			irradiance += textureLod(u_EnvironmentMap, direction, source_mip).rgb * (ndl * sin_theta);
		}
	}

	/* 权重 λ = ΔθΔφ·sinθ·cosθ；再除 π 换成"平均入射辐亮度"口径
	 * （合成侧 ComposeIBLDiffuse 直接乘 albedo，不再除 π） */
	irradiance *= (k_SampleDelta * k_SampleDelta) / PI;
	OutFragColor = vec4(irradiance, 1.0f);
}
