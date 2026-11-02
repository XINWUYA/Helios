#ifndef TERRAIN_COMMON_GLSL
#define TERRAIN_COMMON_GLSL

#include "../Builtin/Uniforms.glsl"

/* 地形的逐绘制参数：高度图与覆盖范围由 C++ 侧按地形实例给（per-draw 覆盖），
 * 不写进材质 —— 同一份材质可以由多块地形共用。 */
uniform sampler2D u_HeightMap;
uniform vec3 u_TerrainOrigin;  /* 地形中心的世界坐标 */
uniform vec2 u_TerrainExtent;  /* 半宽半长（米） */
uniform vec2 u_HeightmapSize;  /* 高度图边长（纹素） */
uniform float u_HeightScale;   /* 高度幅度（米） */
/* morph 带宽的缩放：1 = 边长的四分之一，0 = 只剩一个顶点间距（检查程序用来对照"没有 morph"） */
uniform float u_TerrainMorphScale = 1.0f;

/* 双线性采样世界高度：手动 texelFetch 插值，两个后端取到同一个值。
 * 依赖驱动的 sampler LOD 在粗 LOD 下会各取各的 mip，接缝处对不上。 */
float TerrainHeight(vec2 world_xz)
{
	const vec2 half_extent = max(u_TerrainExtent, vec2(1e-3f));
	/* 世界坐标 → 高度图 UV：原点在地形中心，所以先归一化到 [-1,1] 再折到 [0,1] */
	vec2 uv = (world_xz - u_TerrainOrigin.xz) / (half_extent * 2.0f) + 0.5f;
	uv = clamp(uv, vec2(0.0f), vec2(1.0f));

	const vec2 max_index = max(u_HeightmapSize - vec2(1.0f), vec2(1.0f));
	const vec2 texel = uv * max_index;
	const vec2 base = floor(texel);
	const vec2 frac = texel - base;

	const ivec2 size = ivec2(u_HeightmapSize);
	const ivec2 i0 = ivec2(base);
	const ivec2 i1 = min(i0 + 1, size - 1);

	const float h00 = texelFetch(u_HeightMap, ivec2(i0.x, i0.y), 0).r;
	const float h10 = texelFetch(u_HeightMap, ivec2(i1.x, i0.y), 0).r;
	const float h01 = texelFetch(u_HeightMap, ivec2(i0.x, i1.y), 0).r;
	const float h11 = texelFetch(u_HeightMap, ivec2(i1.x, i1.y), 0).r;

	/* 高度图存 0..1，直接映射到 0..HeightScale：最低点在实体位置，整体向上抬 */
	return mix(mix(h00, h10, frac.x), mix(h01, h11, frac.x), frac.y) * u_HeightScale;
}

/* 世界空间法线：邻居差分重建，step 是这个 LOD 的采样半径（米）。
 * 粗 LOD 用大 step，否则法线会跟着高度图的细节抖出锯齿。 */
vec3 TerrainNormal(vec2 world_xz, float step)
{
	const float hl = TerrainHeight(world_xz - vec2(step, 0.0f));
	const float hr = TerrainHeight(world_xz + vec2(step, 0.0f));
	const float hd = TerrainHeight(world_xz - vec2(0.0f, step));
	const float hu = TerrainHeight(world_xz + vec2(0.0f, step));
	return normalize(vec3(hl - hr, 2.0f * step, hd - hu));
}

/* 粗一档的表面：在 2×step 的网格上取样再线性插值，网格锚在地形原点上。
 * 父节点的顶点正好落在这些格点上，所以边界处父子取到同一个高度，切换 LOD 不会跳。 */
float TerrainHeightCoarse(vec2 world_xz, float step)
{
	const vec2 local = (world_xz - u_TerrainOrigin.xz) / step;
	const vec2 cell = floor(local);
	const vec2 frac = local - cell;
	const vec2 base = u_TerrainOrigin.xz + cell * step;

	const float h00 = TerrainHeight(base);
	const float h10 = TerrainHeight(base + vec2(step, 0.0f));
	const float h01 = TerrainHeight(base + vec2(0.0f, step));
	const float h11 = TerrainHeight(base + vec2(step, step));
	return mix(mix(h00, h10, frac.x), mix(h01, h11, frac.x), frac.y);
}

/* 顶点阶段公共体：由 UV 与 patch 信息算世界位置。
 * a_PatchInfo 的 xyz 是 patch 左下角、w 是边长；a_PatchLod.y 是这个 LOD 的顶点间距（米）。
 * morph 把靠近 patch 边界的顶点拉向粗一档的表面：边界处（morph = 0）父子顶点逐点相等，
 * 内部（morph = 1）用细表面 —— LOD 切换是连续的，不再靠裙边遮。
 * 模板网格比 patch 多一圈顶点（UV 从 -1/32 到 33/32），那一圈仍拉成裙边兜底：
 * 四叉树没有 2:1 平衡，相邻节点可能差两档，morph 只保证差一档的情况。 */
vec3 TerrainVertexWorld(vec2 patch_uv, vec4 patch_info, float lod_step, out float morph)
{
	const float step = max(lod_step, 0.05f);
	vec3 world = patch_info.xyz + vec3(patch_uv.x, 0.0f, patch_uv.y) * patch_info.w;

	const bool outside = patch_uv.x < 0.0f || patch_uv.x > 1.0f
		|| patch_uv.y < 0.0f || patch_uv.y > 1.0f;
	if (outside)
	{
		/* 裙边那一圈不参与 morph：直接按细表面拉下去 */
		morph = 1.0f;
		world.y = TerrainHeight(world.xz) - max(2.0f, patch_info.w * 0.03f);
		return world;
	}

	const float height = TerrainHeight(world.xz);

	/* 到 patch 边界的世界距离，往内一个 morph 带宽（边长的四分之一）过渡到细表面 */
	const float band = patch_info.w * 0.25f * u_TerrainMorphScale;
	if (band <= step)
	{
		/* 缩到没有带宽 = 关掉 morph（检查程序拿它做对照），接缝全靠裙边 */
		morph = 1.0f;
		world.y = height;
		return world;
	}

	const vec2 edge = min(patch_uv, vec2(1.0f) - patch_uv) * patch_info.w;
	const float t = clamp(min(edge.x, edge.y) / band, 0.0f, 1.0f);
	morph = t * t * (3.0f - 2.0f * t);

	world.y = morph < 0.999f
		? mix(TerrainHeightCoarse(world.xz, step * 2.0f), height, morph)
		: height;
	return world;
}

#endif // TERRAIN_COMMON_GLSL
