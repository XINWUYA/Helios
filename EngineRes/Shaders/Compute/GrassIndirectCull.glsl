#type compute
#version 450 core

/* 草地 GPU 剔除 + 实例压实：把可见的叶片抄进另一块缓冲，顺便把间接绘制参数写好。
 * 跟着色器无关的数学跟 ScenePass 的 GrassDensityScale 一致（同样的距离抽稀），
 * 所以开关前后画面应当完全一样。
 *
 * 块的包围球从块包围球表里取，不在这里现推：贴地时块中心跟着地形起伏，
 * 按草地平面推出来的中心会比真实位置低几十米，视锥下边界会把近处的块整片判掉。
 *
 * 参数块与 GrassField::PrepareIndirect 的 C++ 结构逐字节对应。 */

layout(local_size_x = 64) in;

layout(std140, binding = 4) uniform GrassCullBlock
{
	vec4 u_Planes[6];   /* 视锥六个平面（未归一化，按支撑函数判） */
	vec4 u_Counts;      /* x 每块株数、y 总株数、z 抽稀起点、w 剔除距离 */
	vec4 u_Extra;       /* x 最小密度比例、y 全局密度系数 */
	vec4 u_Camera;      /* xyz 相机位置 */
};

layout(std430, binding = 1) readonly buffer SourceInstances
{
	vec4 u_Source[];
};

layout(std430, binding = 2) writeonly buffer DrawInstances
{
	vec4 u_Draw[];
};

/* 间接参数：count / instanceCount / firstIndex / baseVertex / baseInstance */
layout(std430, binding = 3) buffer DrawArgs
{
	uint u_Args[5];
};

/* 压实块号 → 块包围球：xyz 中心（已带地形高度）、w 半径 */
layout(std430, binding = 5) readonly buffer ChunkBounds
{
	vec4 u_ChunkBounds[];
};

/* 抽稀曲线与可见着色共用一份，两边丢的是同一批株 */
#include "../Builtin/GrassDensity.glsl"

void main()
{
	const uint instance = gl_GlobalInvocationID.x;
	if (instance >= uint(u_Counts.y))
		return;

	const uint per_chunk = uint(u_Counts.x);
	const uint packed_chunk = instance / per_chunk;
	const vec4 bounds = u_ChunkBounds[packed_chunk];
	const vec3 center = bounds.xyz;
	const float radius = bounds.w;

	/* 视锥剔除：平面没归一化，按 |n| * r 判 */
	for (int plane = 0; plane < 6; ++plane)
	{
		const vec3 normal = u_Planes[plane].xyz;
		if (dot(normal, center) + u_Planes[plane].w + length(normal) * radius < 0.0f)
			return;
	}

	/* 距离按到块表面的最近距离算：整块都在剔除距离之外才丢。
	 * 块内的稀稠不在这里截断，改由下面逐株判 */
	if (max(length(u_Camera.xyz - center) - radius, 0.0f) >= u_Counts.w)
		return;

	const uint source_base = instance * 5u;

	/* 距离抽稀逐株判：块内密度连续，块边界不出台阶。可见着色里还有同样的一次，
	 * 两边用同一条曲线；这里先丢一遍是为了缩小压实后的缓冲 */
	const vec3 blade_base = u_Source[source_base + 3u].xyz;
	const float blade_distance = length(u_Camera.xyz - blade_base);
	if (GrassDensityKeep(blade_distance, GrassDensityDither(u_Source[source_base + 4u]),
			u_Counts.z, u_Counts.w, u_Extra.x, u_Extra.y) < 0.5f)
		return;

	/* 压实：原子计数器给出这一条在目标缓冲里的位置 */
	const uint slot = atomicAdd(u_Args[1], 1u);
	const uint draw_base = slot * 5u;
	for (uint item = 0u; item < 5u; ++item)
		u_Draw[draw_base + item] = u_Source[source_base + item];
}
