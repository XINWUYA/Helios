#type compute
#version 450 core

/* 草地 GPU 剔除 + 实例压实：把可见的叶片抄进另一块缓冲，顺便把间接绘制参数写好。
 * 跟着色器无关的数学跟 ScenePass 的 GrassDensityScale 一致（同样的距离抽稀），
 * 所以开关前后画面应当完全一样。
 *
 * 参数块与 GrassField::PrepareIndirect 的 C++ 结构逐字节对应。 */

layout(local_size_x = 64) in;

layout(std140, binding = 4) uniform GrassCullBlock
{
	vec4 u_Planes[6];   /* 视锥六个平面（未归一化，按支撑函数判） */
	vec4 u_Origin;      /* xyz 场地原点 */
	vec4 u_Right;       /* xyz 右向轴，w = 半宽 */
	vec4 u_Forward;     /* xyz 前向轴，w = 半长 */
	vec4 u_Grid;        /* x 块宽、y 块深、z 每块株数、w 总株数 */
	vec4 u_Counts;      /* x 块数 x、y 块数 z、z 抽稀起点、w 剔除距离 */
	vec4 u_Extra;       /* x 最小密度比例、y 草高 */
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

void main()
{
	const uint instance = gl_GlobalInvocationID.x;
	if (instance >= uint(u_Grid.w))
		return;

	const uint per_chunk = uint(u_Grid.z);
	const uint chunk_index = instance / per_chunk;
	const uint chunk_x = chunk_index % uint(u_Counts.x);
	const uint chunk_z = chunk_index / uint(u_Counts.x);

	/* 块中心与包围半径：块是 8 米见方的格子，中心由原点沿两个轴推出来 */
	const vec3 center = u_Origin.xyz
		+ u_Right.xyz * (-u_Right.w + (float(chunk_x) + 0.5f) * u_Grid.x)
		+ u_Forward.xyz * (-u_Forward.w + (float(chunk_z) + 0.5f) * u_Grid.y)
		+ vec3(0.0f, u_Extra.y * 0.5f, 0.0f);
	const float radius = 0.5f * length(vec2(u_Grid.x, u_Grid.y)) + u_Extra.y;

	/* 视锥剔除：平面没归一化，按 |n| * r 判 */
	for (int plane = 0; plane < 6; ++plane)
	{
		const vec3 normal = u_Planes[plane].xyz;
		if (dot(normal, center) + u_Planes[plane].w + length(normal) * radius < 0.0f)
			return;
	}

	/* 距离抽稀：跟可见路径同一条曲线 */
	const float view_distance = length(u_Camera.xyz - center);
	if (view_distance >= u_Counts.w)
		return;

	float scale = 1.0f;
	if (view_distance > u_Counts.z)
	{
		const float span = max(u_Counts.w - u_Counts.z, 1e-3f);
		const float t = clamp((view_distance - u_Counts.z) / span, 0.0f, 1.0f);
		scale = mix(1.0f, max(u_Extra.x, 0.0f), t * t);
	}

	/* 块内只有前 draw_count 条要画：块内顺序本来就是随机的，取前缀等于均匀抽稀 */
	const uint blade_index = instance - chunk_index * per_chunk;
	const uint draw_count = max(1u, uint(float(per_chunk) * scale));
	if (blade_index >= draw_count)
		return;

	/* 压实：原子计数器给出这一条在目标缓冲里的位置 */
	const uint slot = atomicAdd(u_Args[1], 1u);
	const uint source_base = instance * 5u;
	const uint draw_base = slot * 5u;
	for (uint item = 0u; item < 5u; ++item)
		u_Draw[draw_base + item] = u_Source[source_base + item];
}
