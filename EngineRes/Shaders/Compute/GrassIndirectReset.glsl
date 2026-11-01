#type compute
#version 450 core

/* 每小时重置一次间接绘制参数：索引数与实例数由剔除那一步填，
 * 这里只把计数清到「索引数 / 0 实例」的初始状态。 */

layout(local_size_x = 1) in;

layout(std140, binding = 4) uniform GrassArgsResetBlock
{
	uint u_IndexCount;   /* 一个叶片模板有多少个索引 */
};

layout(std430, binding = 3) buffer DrawArgs
{
	uint u_Args[5];
};

void main()
{
	u_Args[0] = u_IndexCount;   /* count */
	u_Args[1] = 0u;             /* instanceCount：交给剔除那一步累加 */
	u_Args[2] = 0u;             /* firstIndex */
	u_Args[3] = 0u;             /* baseVertex */
	u_Args[4] = 0u;             /* baseInstance */
}
