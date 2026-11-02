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

	/* 高度图存 0..1，映射到以实体位置为中心的 ±HeightScale/2：
	 * 实体在原点时地形上下各一半，场景里 y=0 的物体不会被整块埋掉 */
	return (mix(mix(h00, h10, frac.x), mix(h01, h11, frac.x), frac.y) - 0.5f) * u_HeightScale;
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

/* 顶点阶段公共体：由 UV 与 patch 信息算世界位置。
 * a_PatchInfo 的 xyz 是 patch 左下角、w 是边长；LOD 参数在 y 里（当前实现只用到采样步长）。
 * 模板网格比 patch 多一圈顶点（UV 从 -1/32 到 33/32），超出 [0,1] 的那圈向下拉成裙边，
 * 用来盖住相邻 LOD 之间的接缝（morph 接缝还没做，见方案文档 P3）。 */
vec3 TerrainVertexWorld(vec2 patch_uv, vec4 patch_info)
{
	vec3 world = patch_info.xyz + vec3(patch_uv.x, 0.0f, patch_uv.y) * patch_info.w;
	world.y = TerrainHeight(world.xz);

	const bool outside = patch_uv.x < 0.0f || patch_uv.x > 1.0f
		|| patch_uv.y < 0.0f || patch_uv.y > 1.0f;
	if (outside)
		world.y -= max(2.0f, patch_info.w * 0.03f);

	return world;
}

#endif // TERRAIN_COMMON_GLSL
