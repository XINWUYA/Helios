#type compute
#version 450 core

/* 草地的块级剔除：一块测一次光源盒，通过的压进可见块表，逐株剔除只扫表里的块。
 * 表头 [0] 是可见块数（同时当发号计数器），[1..] 是压实块号；派发参数由最后
 * 完成的线程写，它读到的可见块数已含全部 append。平面在 CPU 侧已归一化。 */

layout(local_size_x = 64) in;

layout(std140, binding = 4) uniform GrassChunkCullBlock
{
	vec4 u_Planes[6];
	vec4 u_Eye;       /* xyz 观察点、w 抽稀终点 */
	uvec4 u_Counts;   /* x 块数、y 每块株数、z 派发线程数、w 总株数 */
	uvec4 u_Layout;   /* x 每段参数占几个 uint、y 段数、z 曲线（1 = 草影） */
};

/* 压实块号 → 块包围球：xyz 中心（已带地形高度）、w 半径 */
layout(std430, binding = 5) readonly buffer ChunkBounds
{
	vec4 u_Bounds[];
};

layout(std430, binding = 6) buffer ChunkList
{
	uint u_Chunks[];
};

/* 间接参数：每段 5 个 uint，段之后是发号计数、完成计数与派发参数 */
layout(std430, binding = 3) buffer DrawArgs
{
	uint u_Args[];
};

void main()
{
	const uint chunk = gl_GlobalInvocationID.x;
	if (chunk < u_Counts.x)
	{
		const vec4 bounds = u_Bounds[chunk];
		bool visible = true;

		/* 视锥剔除：平面已归一化，按 dot(n, c) + w + r 判 */
		for (int plane = 0; plane < 6; ++plane)
		{
			if (dot(u_Planes[plane].xyz, bounds.xyz) + u_Planes[plane].w + bounds.w < 0.0f)
			{
				visible = false;
				break;
			}
		}

		/* 距离圈：整块都在抽稀终点之外就不用进表。
		 * 草影按水平距离，跟 GrassShadow.glsl 一个口径 */
		if (visible)
		{
			const float reach = (u_Layout.z != 0u)
				? length(u_Eye.xz - bounds.xyz.xz) - bounds.w
				: length(u_Eye.xyz - bounds.xyz) - bounds.w;
			visible = max(reach, 0.0f) < u_Eye.w;
		}

		if (visible)
			u_Chunks[atomicAdd(u_Chunks[0], 1u) + 1u] = chunk;
	}

	/* 最后一个完成的线程写间接派发参数：此时所有 append 都已经落盘 */
	const uint header = u_Layout.y * u_Layout.x;
	const uint done = atomicAdd(u_Args[header + 2u], 1u);
	if (done + 1u == u_Counts.z)
	{
		const uint visible = u_Chunks[0];
		u_Args[header + 4u] = (visible * u_Counts.y + 63u) / 64u;
		u_Args[header + 5u] = 1u;
		u_Args[header + 6u] = 1u;
	}
}
