#type compute
#version 450 core

/* 草地 GPU 剔除 + 实例压实：把可见的叶片抄进目标缓冲，顺便把间接绘制参数写好。
 * 相机和每一档级联各派发一次，各写各的槽位；目标缓冲按段切分，绘制端逐段提交。
 *
 * GRASS_CULL_CHUNKED 只换头部：1 只扫块级剔除压出来的可见块表，近处级联不陪跑整片；
 * 0 逐实例自己测视锥，当作对照。抽稀、发号、压实两边共用一套。
 * 参数块与 GrassField::PrepareIndirect 的 C++ 结构逐字节对应。 */

/* 0 = 逐实例测视锥，1 = 只扫块级剔除留下的可见块表。默认值由引擎按声明补 0，
 * 这里不再写 #ifndef 兜底；判断只用 #if。 */
// @variant(GRASS_CULL_CHUNKED, external)

layout(local_size_x = 64) in;

#if GRASS_CULL_CHUNKED
layout(std140, binding = 4) uniform GrassCullBlock
{
	vec4 u_Counts;      /* x 每块株数、y 总株数、z 抽稀起点、w 抽稀终点 */
	vec4 u_Extra;       /* x 最小密度比例、y 全局密度系数、z 预算系数、w 曲线 */
	vec4 u_Camera;      /* xyz 观察点 */
	uvec4 u_Segments;   /* x 段数、y 每段株数、z 参数步长 */
};
#else
layout(std140, binding = 4) uniform GrassCullFullBlock
{
	vec4 u_Planes[6];   /* 视锥六平面，CPU 侧已归一化 */
	vec4 u_Counts;
	vec4 u_Extra;
	vec4 u_Camera;
	uvec4 u_Segments;
};
#endif

layout(std430, binding = 1) readonly buffer SourceInstances
{
	vec4 u_Source[];
};

layout(std430, binding = 2) writeonly buffer DrawInstances
{
	vec4 u_Draw[];
};

/* 间接参数：每段 5 个 uint，段后跟发号计数、完成计数与派发参数 */
layout(std430, binding = 3) buffer DrawArgs
{
	uint u_Args[];
};

#if GRASS_CULL_CHUNKED
/* 可见块表：[0] 可见块数、[1..] 压实块号 */
layout(std430, binding = 5) readonly buffer ChunkList
{
	uint u_Chunks[];
};
#else
/* 压实块号 → 块包围球：xyz 中心（已带地形高度）、w 半径 */
layout(std430, binding = 5) readonly buffer ChunkBounds
{
	vec4 u_Bounds[];
};
#endif

/* 抽稀曲线与可见着色、阴影着色共用一份，几边丢的是同一批株 */
#include "GrassDensity.glsl"

void main()
{
	/* 草影视图按水平距离抽稀，可见视图按到叶片的直线距离 */
	const bool shadow_view = u_Extra.w > 0.5f;

#if GRASS_CULL_CHUNKED
	const uint per_chunk = max(uint(u_Counts.x), 1u);
	const uint index = gl_GlobalInvocationID.x;
	const uint chunk_slot = index / per_chunk;

	/* 派发按可见块数算，尾部的组会落在这里直接退掉 */
	if (chunk_slot >= u_Chunks[0])
		return;

	const uint instance = u_Chunks[chunk_slot + 1u] * per_chunk + index % per_chunk;
	if (instance >= uint(u_Counts.y))
		return;
#else
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

	/* 距离按到块表面的最近距离算：整块都在抽稀终点之外才丢 */
	const float reach = shadow_view
		? length(u_Camera.xz - bounds.xyz.xz) - bounds.w
		: length(u_Camera.xyz - bounds.xyz) - bounds.w;
	if (max(reach, 0.0f) >= u_Counts.w)
		return;
#endif

	const uint source_base = instance * 5u;
	const vec3 blade_base = u_Source[source_base + 3u].xyz;
	const float dither = GrassDensityDither(u_Source[source_base + 4u]);

	/* 逐株判：块内密度连续，块边界不出台阶。
	 * 绘制端还会用同一条曲线再判一次，两边丢的是同一批株 */
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

	/* 压实：段内偏移由号数取余给出，实例数按段各自累加 */
	const uint draw_base = (segment * capacity + ticket % capacity) * 5u;
	for (uint item = 0u; item < 5u; ++item)
		u_Draw[draw_base + item] = u_Source[source_base + item];
	atomicAdd(u_Args[segment * stride + 1u], 1u);
}
