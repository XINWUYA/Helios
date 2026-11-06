#type compute
#version 450 core

/* 全量剔除：不做块级预剔，逐实例自己测视锥。掩码开关关掉时走这条，
 * 用来对比「先压块表再扫」到底省了多少。参数块与 GrassField::PrepareIndirect
 * 的 C++ 结构逐字节对应，平面在 CPU 侧已归一化。 */

layout(local_size_x = 64) in;

layout(std140, binding = 4) uniform GrassCullFullBlock
{
	vec4 u_Planes[6];
	vec4 u_Counts;      /* x 每块株数、y 总株数、z 抽稀起点、w 抽稀终点 */
	vec4 u_Extra;       /* x 最小密度比例、y 全局密度系数、z 预算系数、w 曲线 */
	vec4 u_Camera;      /* xyz 观察点 */
	uvec4 u_Segments;   /* x 段数、y 每段株数、z 参数步长 */
};

layout(std430, binding = 1) readonly buffer SourceInstances
{
	vec4 u_Source[];
};

layout(std430, binding = 2) writeonly buffer DrawInstances
{
	vec4 u_Draw[];
};

layout(std430, binding = 3) buffer DrawArgs
{
	uint u_Args[];
};

/* 压实块号 → 块包围球：xyz 中心（已带地形高度）、w 半径 */
layout(std430, binding = 5) readonly buffer ChunkBounds
{
	vec4 u_Bounds[];
};

/* 抽稀曲线与可见着色、阴影着色共用一份，几边丢的是同一批株 */
#include "../Builtin/GrassDensity.glsl"

void main()
{
	const uint instance = gl_GlobalInvocationID.x;
	if (instance >= uint(u_Counts.y))
		return;

	const uint per_chunk = max(uint(u_Counts.x), 1u);
	const vec4 bounds = u_Bounds[instance / per_chunk];

	/* 视锥剔除：平面已归一化，按 dot(n, c) + w + r 判 */
	for (int plane = 0; plane < 6; ++plane)
	{
		if (dot(u_Planes[plane].xyz, bounds.xyz) + u_Planes[plane].w + bounds.w < 0.0f)
			return;
	}

	const bool shadow_view = u_Extra.w > 0.5f;

	/* 距离按到块表面的最近距离算：整块都在抽稀终点之外才丢。
	 * 草影按水平距离，跟 GrassShadow.glsl 一个口径 */
	const float reach = (shadow_view)
		? length(u_Camera.xz - bounds.xyz.xz) - bounds.w
		: length(u_Camera.xyz - bounds.xyz) - bounds.w;
	if (max(reach, 0.0f) >= u_Counts.w)
		return;

	const uint source_base = instance * 5u;
	const vec3 blade_base = u_Source[source_base + 3u].xyz;
	const float dither = GrassDensityDither(u_Source[source_base + 4u]);

	/* 逐株判：块内密度连续，块边界不出台阶 */
	float weight;
	if (shadow_view)
	{
		weight = GrassShadowWeight(length(u_Camera.xz - blade_base.xz), u_Counts.z, u_Counts.w)
			* u_Extra.y * u_Extra.z;
	}
	else
	{
		weight = GrassDensityWeight(length(u_Camera.xyz - blade_base), u_Counts.z, u_Counts.w,
			u_Extra.x) * u_Extra.y;
	}
	if (dither > weight)
		return;

	/* 分段：一个计数器从前往后发号，装满一段换下一段，段内不留空洞 */
	const uint segments = max(u_Segments.x, 1u);
	const uint capacity = max(u_Segments.y, 1u);
	const uint stride = u_Segments.z;
	const uint ticket = atomicAdd(u_Args[segments * stride], 1u);
	const uint segment = ticket / capacity;
	if (segment >= segments)
		return;

	const uint draw_base = (segment * capacity + ticket % capacity) * 5u;
	for (uint item = 0u; item < 5u; ++item)
		u_Draw[draw_base + item] = u_Source[source_base + item];
	atomicAdd(u_Args[segment * stride + 1u], 1u);
}
