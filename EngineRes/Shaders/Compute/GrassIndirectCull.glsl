#type compute
#version 450 core

/* 草地 GPU 剔除 + 实例压实：把可见的叶片抄进目标缓冲，顺便把间接绘制参数写好。
 * 相机和每一档级联各派发一次，各写各的槽位。
 *
 * 只扫块级剔除留下的可见块表：近处级联只跑它覆盖到的那几块，不再陪跑整片。
 * 目标缓冲按段切分，一段装满换下一段，绘制端逐段提交。
 *
 * 参数块与 GrassField::PrepareIndirect 的 C++ 结构逐字节对应。 */

layout(local_size_x = 64) in;

layout(std140, binding = 4) uniform GrassCullBlock
{
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

/* 间接参数：每段 5 个 uint，段后跟发号计数、完成计数与派发参数 */
layout(std430, binding = 3) buffer DrawArgs
{
	uint u_Args[];
};

/* 可见块表：[0] 可见块数、[1..] 压实块号 */
layout(std430, binding = 5) readonly buffer ChunkList
{
	uint u_Chunks[];
};

/* 抽稀曲线与可见着色、阴影着色共用一份，几边丢的是同一批株 */
#include "../Builtin/GrassDensity.glsl"

void main()
{
	const uint per_chunk = max(uint(u_Counts.x), 1u);
	const uint index = gl_GlobalInvocationID.x;
	const uint chunk_slot = index / per_chunk;

	/* 派发按可见块数算，尾部的组会落在这里直接退掉 */
	if (chunk_slot >= u_Chunks[0])
		return;

	const uint instance = u_Chunks[chunk_slot + 1u] * per_chunk + index % per_chunk;
	if (instance >= uint(u_Counts.y))
		return;

	const uint source_base = instance * 5u;
	const vec3 blade_base = u_Source[source_base + 3u].xyz;
	const float dither = GrassDensityDither(u_Source[source_base + 4u]);

	/* 逐株判：块内密度连续，块边界不出台阶。
	 * 绘制端还会用同一条曲线再判一次，两边丢的是同一批株 */
	float weight;
	if (u_Extra.w > 0.5f)
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
