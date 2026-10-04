#type vertex
#version 410 core

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

layout(binding = 0) uniform sampler2D u_EquirectangularMap;
layout(location = 0) uniform int u_FaceId;

/* 将方向映射到等距柱状(equirectangular)贴图 UV：水平环绕 360°(经度)，
 * 垂直按纬度（asin）映射。必须与 SkyBox.glsl 的 SampleSphericalMap 保持一致，
 * 否则探针烘焙出的环境与屏幕天空盒在垂直方向对不上（改动其一需同步另一处）。 */
vec2 SampleEquirectangularMap(vec3 direction)
{
	float theta = atan(direction.z, direction.x); // -PI..PI
	float u = theta * INV_2PI + 0.5f;
	float v = asin(clamp(direction.y, -1.0f, 1.0f)) * INV_PI + 0.5f;
	return vec2(u, v);
}

void main()
{
	vec3 dir = GetCubeFaceDirection(u_FaceId, WorldPosition.xy);
	vec2 equirect_uv = SampleEquirectangularMap(dir);
	vec3 color = texture(u_EquirectangularMap, equirect_uv).rgb;

	/* 辐亮度上限：源 HDR 的太阳核心高达 1.4e5，超过 RGBA16F 上限会被硬钳到饱和，
	 * 并被无条件计入后续所有卷积（与方向光重复计算太阳）。引擎用 DirectionalLight
	 * 单独表达太阳，这里把环境图里的太阳压到安全上限即可。 */
	const float kMaxRadiance = 200.0f;
	OutColor = vec4(min(color, vec3(kMaxRadiance)), 1.0f);
}
