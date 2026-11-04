#type vertex
#version 410 core

layout(location = 0) in vec4 a_Position;
out vec2 v_TexCoord;

void main()
{
	gl_Position = a_Position;
	v_TexCoord = a_Position.xy * 0.5f + 0.5f;
}


#type fragment
// @depth-texture u_Depth
#version 410 core

/* 解析高度雾：密度随高度指数衰减，沿视线直接积分光学厚度，不用步进。
 * 雾色由 CPU 按天空参数算好，这里再叠一层朝太阳的相函数散射。
 * 天空照样吃雾，光程按积到无穷远算，仰角越低越浓、天顶基本不动。 */

#include "../Builtin/Uniforms.glsl"
#include "../Builtin/SkyUniforms.glsl"

in vec2 v_TexCoord;
layout(location = 0) out vec4 OutFragColor;

uniform sampler2D u_SceneColor;
uniform sampler2D u_Depth;
uniform mat4 u_InvViewProjection;

uniform vec4 u_FogColorOpacity; /* rgb 雾色, a 最大不透明度 */
uniform vec4 u_FogShape;        /* x 密度, y 高度衰减, z 基准高度, w 起始距离 */
uniform vec4 u_FogScatter;      /* x 太阳散射强度, y 各向异性 g, z 天空受雾权重 */

/* 天空的光程按无穷远算：密度随高度指数衰减，积下来仰角越低越浓、天顶几乎不动。
 * 换成固定的天空距离会把天顶一起糊成雾色。 */
const float kSkyDistance = 1.0e6f;

/* 天空辐照度的量级远大于雾，压到雾的亮度档再用 */
const float kSunScatterScale = 0.012f;

/* 归一化相函数：g = 0 时返回 1，前向峰值随 g 变大 */
float PhaseHG(float cos_theta, float g)
{
	const float g2 = g * g;
	const float denom = max(1.0f + g2 - 2.0f * g * cos_theta, 1.0e-4f);
	return (1.0f - g2) / pow(denom, 1.5f);
}

/* 从 start 到 end 沿视线积分 rho0 * exp(-k * y)。
 * 返回光学厚度，Vy 接近 0 时取极限，避免除零。 */
float OpticalDepth(float start, float end, float dir_y, float base_density, float k)
{
	const float segment = max(end - start, 0.0f);
	if (segment <= 0.0f)
		return 0.0f;

	const float a = k * dir_y;
	const float factor = abs(a) < 1.0e-4f ? 1.0f : (1.0f - exp(-a * segment)) / (a * segment);
	return base_density * exp(-a * start) * segment * factor;
}

void main()
{
	const vec3 scene_color = texture(u_SceneColor, v_TexCoord).rgb;

	/* 远平面（Reversed-Z 下是 0）是天空：照样吃雾，只是光程按无穷远算 */
	const float depth = texture(u_Depth, v_TexCoord).r;
	const float sky = depth <= 1.0e-7f ? 1.0f : 0.0f;

	/* 深度反投影回世界拿沿视线距离。天空反投影不出有限距离，只借它取方向，先换到中间平面。
	 * 引擎是 ZO + Reversed-Z（NDC z ∈ [0,1]，近 1 远 0，见 Math.h），clip.z 直接取 depth；
	 * 按 GL 习惯写成 depth * 2 - 1 会把整屏当成贴近平面的近处，雾永远出不来。 */
	const vec4 clip = vec4(v_TexCoord * 2.0f - 1.0f, mix(depth, 0.5f, sky), 1.0f);
	const vec4 world_h = u_InvViewProjection * clip;
	const vec3 world = world_h.xyz / world_h.w;

	const vec3 ray = world - u_ViewPos;
	const float ray_length = length(ray);
	const vec3 dir = ray / max(ray_length, 1.0e-4f);
	const float distance = mix(ray_length, kSkyDistance, sky);

	/* 高度雾的解析光学厚度：k = 0 时退化成均匀雾 */
	const float k = max(u_FogShape.y, 0.0f);
	const float base_density = u_FogShape.x * min(exp(-k * (u_ViewPos.y - u_FogShape.z)), 50.0f);
	const float start = clamp(u_FogShape.w, 0.0f, distance);
	/* 天空的段长是 1e6，贴地平线时 factor 会溢出到 inf，压一档免得后面算出 NaN */
	const float tau = min(OpticalDepth(start, distance, dir.y, base_density, k), 30.0f);

	/* MaxOpacity：最多遮到这么多，剩下始终保留一点场景色 */
	float transmittance = exp(-tau);
	transmittance = mix(1.0f, transmittance, clamp(u_FogColorOpacity.a, 0.0f, 1.0f));
	/* SkyAffect：天空受雾的权重。0 是「天空自己带大气色」的旧行为，1 与地面同一套 */
	transmittance = mix(1.0f, transmittance, mix(1.0f, clamp(u_FogScatter.z, 0.0f, 1.0f), sky));

	/* 朝太阳方向的单次散射：雾里的光柱感来自这一项 */
	const float cos_theta = dot(dir, u_SkySunDirection.xyz);
	const float g = clamp(u_FogScatter.y, -0.9f, 0.9f);
	const vec3 sun_scatter = u_SkySunRadiance.rgb
		* (kSunScatterScale * u_FogScatter.x * PhaseHG(cos_theta, g));

	const vec3 inscatter = u_FogColorOpacity.rgb + sun_scatter;
	OutFragColor = vec4(scene_color * transmittance + inscatter * (1.0f - transmittance), 1.0f);
}
