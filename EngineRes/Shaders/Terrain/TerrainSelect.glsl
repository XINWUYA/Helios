#type compute
#version 450 core

/* 地形 patch 的 GPU 选择：按 CPU 四叉树那套判据把可见节点压成实例，
 * 顺带写好间接绘制参数与每边的 morph 掩码。
 * 节点表是一棵按层摊平的满四叉树，所以「某个点被哪一档节点盖着」直接按块号查，
 * 不用另建邻居表。参数块与 TerrainField::PrepareSelection 的 C++ 结构逐字节对应。 */

layout(local_size_x = 64) in;

layout(std140, binding = 4) uniform SelectBlock
{
	mat4 u_Model;      /* 局部 → 世界 */
	vec4 u_Planes[6];  /* 视锥六个平面（未归一化，按支撑函数判） */
	vec4 u_Eye;        /* xyz 相机位置，w 节点总数 */
	vec4 u_Counts;     /* x 根节点边长、y 最细节点边长、z LOD 比例、w 世界缩放 */
	vec4 u_Extra;      /* y 最大级数，其余保留 */
};

layout(std430, binding = 1) readonly buffer NodeTable
{
	vec4 u_Nodes[];    /* 每节点两条：xy 局部中心、z 边长、w 级数；xy 局部高度上下界 */
};

layout(std430, binding = 2) writeonly buffer PatchInstances
{
	vec4 u_Patches[];  /* 每 patch 两条：Info 与 Lod，与 TerrainPatch 逐字节一致 */
};

layout(std430, binding = 3) buffer DrawArgs
{
	uint u_Args[5];    /* 索引数由 CPU 预设，实例数从这里累加 */
};

/* 满四叉树里第 level 层第 block 块的节点下标：前几层是等比数列 */
uint TerrainNodeIndex(int level, ivec2 block)
{
	const uint side = 1u << uint(level);
	const uint offset = ((1u << uint(2 * level)) - 1u) / 3u;
	return offset + uint(block.y) * side + uint(block.x);
}

/* 节点在世界空间的中心与边长，顺便按包围盒判视锥。盒子取节点自己的高度范围，
 * 局部盒过实体矩阵取轴对齐范围，与 TerrainField::GetNodeWorldBounds 同一套 */
bool TerrainNodeInside(uint index, out vec3 world_center, out float world_size)
{
	const vec4 node = u_Nodes[index * 2u];
	const vec4 heights = u_Nodes[index * 2u + 1u];
	/* node.xy 是局部的 (x, z)：世界位置要把 z 放回第三个分量 */
	world_center = (u_Model * vec4(node.x, (heights.x + heights.y) * 0.5, node.y, 1.0)).xyz;
	world_size = node.z * u_Counts.w;

	const vec3 local_extent = vec3(node.z * 0.5, max((heights.y - heights.x) * 0.5, 0.01), node.z * 0.5);
	const vec3 extent = abs(u_Model[0].xyz) * local_extent.x + abs(u_Model[1].xyz) * local_extent.y
		+ abs(u_Model[2].xyz) * local_extent.z;
	for (int plane = 0; plane < 6; ++plane)
	{
		const vec3 normal = u_Planes[plane].xyz;
		if (dot(normal, world_center) + u_Planes[plane].w + dot(abs(normal), extent) < 0.0)
			return false;
	}
	return true;
}

/* 这个节点会不会继续往下分：与 TerrainField::CollectNodes 同一套判据。
 * 视锥外整块丢；还不到最细、离得又够近就继续分 */
bool TerrainNodeSplits(uint index)
{
	vec3 center;
	float size;
	if (!TerrainNodeInside(index, center, size))
		return false;

	/* 最细一档就是节点表的最后一层：两边都按层数判，表封顶时也对得上 */
	const bool can_split = u_Nodes[index * 2u].w + 0.5 < u_Extra.y;
	return can_split && distance(u_Eye.xyz, center) < size * u_Counts.z;
}

/* 这个节点会不会出 patch：在视锥里、自己不分、父节点分过。
 * 父节点没分的话那一块由父自己画，再画一遍就是同一块地方叠两层面片；
 * 视锥外的节点即使父节点在细分也不画，跟 CPU 直接返回一致 */
bool TerrainNodeEmitted(uint index, out vec3 world_center, out float world_size, out int level)
{
	const vec4 node = u_Nodes[index * 2u];
	level = int(node.w + 0.5);

	if (!TerrainNodeInside(index, world_center, world_size))
		return false;
	if (TerrainNodeSplits(index))
		return false;

	if (level > 0)
	{
		const ivec2 block = ivec2(floor((node.xy + u_Counts.x * 0.5) / node.z));
		if (!TerrainNodeSplits(TerrainNodeIndex(level - 1, block >> 1)))
			return false;
	}
	return true;
}

/* 探针点被哪一档的可见节点盖着：从最细一层往上找。
 * 探针落到视锥外（那一片根本没画）时按 fallback 算，跟 CPU 找不到邻居同一种处理 */
int TerrainCoveringLevel(vec2 local_xz, int fallback)
{
	const int max_level = int(u_Extra.y + 0.5);
	const float cell = max(u_Counts.y, 1e-3);
	const vec2 half_root = vec2(u_Counts.x * 0.5);

	/* 探针跑到地形外（最外一圈 patch 的外侧）就没有邻居可查，按 fallback 算，
	 * 不然块号会折到下一行，读到别处去的节点 */
	if (any(lessThan(local_xz, -half_root)) || any(greaterThan(local_xz, half_root)))
		return fallback;

	const ivec2 finest = ivec2(floor((local_xz + half_root) / cell));

	for (int level = max_level; level >= 0; --level)
	{
		const ivec2 block = finest >> (max_level - level);
		const uint index = TerrainNodeIndex(level, block);
		if (index >= uint(u_Eye.w))
			continue;

		vec3 center;
		float size;
		int node_level;
		if (TerrainNodeEmitted(index, center, size, node_level))
			return level;
	}
	return fallback;
}

void main()
{
	const uint index = gl_GlobalInvocationID.x;
	if (index >= uint(u_Eye.w))
		return;

	vec3 world_center;
	float world_size;
	int level;
	if (!TerrainNodeEmitted(index, world_center, world_size, level))
		return;

	const vec4 node = u_Nodes[index * 2u];
	const vec2 min_corner = node.xy - node.z * 0.5;

	/* 每条边探一格：邻居更粗就往父级表面 morph。位序 1 = -X、2 = +X、4 = -Z、8 = +Z */
	int mask = 0;
	if (level > 0)
	{
		const float probe = 0.01;
		const vec2 mid = node.xy;
		const vec2 probes[4] = {
			vec2(min_corner.x - probe, mid.y),
			vec2(min_corner.x + node.z + probe, mid.y),
			vec2(mid.x, min_corner.y - probe),
			vec2(mid.x, min_corner.y + node.z + probe),
		};
		for (int side = 0; side < 4; ++side)
		{
			if (TerrainCoveringLevel(probes[side], level) < level)
				mask |= 1 << side;
		}
	}

	/* 压实：原子计数器给出这一条在缓冲里的位置 */
	const uint slot = atomicAdd(u_Args[1], 1u);
	const uint base = slot * 2u;
	/* Info 是 xyz = 左下角（局部空间，y 恒为 0）+ w 边长，与 TerrainPatch 逐字节对应 */
	u_Patches[base] = vec4(min_corner.x, 0.0, min_corner.y, node.z);
	/* 采样步长与 CPU 一致：节点边长除以 32 段 */
	u_Patches[base + 1u] = vec4(node.w, node.z / 32.0, float(mask), 0.0);
}
