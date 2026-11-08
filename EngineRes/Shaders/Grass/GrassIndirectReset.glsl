#type compute
#version 450 core

/* 每次剔除前重置间接绘制参数：每段都回到「索引数与 0 实例」的初始状态，
 * 段计数器与可见块表也一起清 —— 后面两步都靠它们从零开始发号 */

layout(local_size_x = 1) in;

layout(std140, binding = 4) uniform GrassArgsResetBlock
{
	uvec4 u_Reset;   /* x 索引数、y 段数、z 每段参数占几个 uint */
};

layout(std430, binding = 3) buffer DrawArgs
{
	uint u_Args[];
};

/* 可见块表：[0] 是可见块数，块级剔除拿它当计数器 */
layout(std430, binding = 6) buffer ChunkList
{
	uint u_Chunks[];
};

void main()
{
	for (uint segment = 0u; segment < u_Reset.y; ++segment)
	{
		const uint base = segment * u_Reset.z;
		u_Args[base + 0u] = u_Reset.x;   /* count */
		u_Args[base + 1u] = 0u;          /* instanceCount：交给剔除那一步累加 */
		u_Args[base + 2u] = 0u;          /* firstIndex */
		u_Args[base + 3u] = 0u;          /* baseVertex */
		u_Args[base + 4u] = 0u;          /* baseInstance */
	}

	const uint header = u_Reset.y * u_Reset.z;
	u_Args[header + 0u] = 0u;   /* 分段发号 */
	u_Args[header + 2u] = 0u;   /* 块级剔除的完成计数 */
	u_Chunks[0] = 0u;
}
