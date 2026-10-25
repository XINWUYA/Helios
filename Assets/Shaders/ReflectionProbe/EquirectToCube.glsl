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

uniform sampler2D u_EquirectangularMap;
uniform int u_FaceId;

/* 方向 → 等距柱状贴图 UV（水平经度环绕 360°、垂直按纬度的 asin 映射）。
 * 必须与 SkyBox.glsl 的 SampleSphericalMap 保持一致（改一处要同步另一处），
 * 否则探针烘焙的环境与屏幕天空盒垂直方向对不上 */
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

	/* 辐亮度上限：源 HDR 的太阳核心远超 RGBA16F 上限、会被硬钳并计入后续所有卷积
	 * （与方向光重复计算太阳）。引擎用 DirectionalLight 单独表达太阳，这里压到安全值即可 */
	const float kMaxRadiance = 200.0f;
	OutFragColor = vec4(min(color, vec3(kMaxRadiance)), 1.0f);
}
