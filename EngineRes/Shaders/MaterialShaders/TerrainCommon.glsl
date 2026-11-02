#ifndef TERRAIN_COMMON_GLSL
#define TERRAIN_COMMON_GLSL

#include "../Builtin/Uniforms.glsl"

/* 地形几何在实体自己的局部空间里：xz 以地形中心为原点，高度沿局部 y。
 * 世界位置由 u_TerrainModel 变换，高度采样与混色都用局部坐标 ——
 * 拖 Transform 只会换矩阵，山形与材质分布不跟着重排。 */
uniform mat4 u_TerrainModel;          /* 局部 → 世界 */
uniform mat4 u_TerrainInverse;        /* 世界 → 局部（页烘焙把世界点拉回来） */
uniform mat4 u_TerrainNormalMatrix;   /* 法线：世界矩阵的逆转置，只用 3x3 */

/* 地形数据：高度图与范围。带 @property 的会进材质参数表（属性面板里直接看），
 * 渲染时由 C++ 按地形逐绘制覆盖 —— 同一份材质仍可被多块地形共用 */
uniform sampler2D u_HeightMap;    // @property
uniform vec2 u_TerrainExtent;     // @property 局部半宽半长（米）
uniform vec2 u_TerrainWorldExtent; /* 世界空间的半宽半长：页表按世界切 */
uniform vec3 u_TerrainOrigin;      /* 地形中心的世界坐标（页表与拾取定位用） */
uniform vec2 u_HeightmapSize;     // @property 高度图边长（纹素）
uniform float u_HeightScale;      // @property 局部高度幅度（米）
/* morph 带宽的缩放：1 = 边长的四分之一，0 = 只剩一个顶点间距（检查程序用来对照"没有 morph"） */
uniform float u_TerrainMorphScale = 1.0f;

/* 双线性采样局部高度：手动 texelFetch 插值，两个后端取到同一个值。
 * 依赖驱动的 sampler LOD 在粗 LOD 下会各取各的 mip，接缝处对不上。 */
float TerrainHeight(vec2 local_xz)
{
	const vec2 half_extent = max(u_TerrainExtent, vec2(1e-3f));
	/* 局部坐标 → 高度图 UV：原点在地形中心，所以先归一化到 [-1,1] 再折到 [0,1] */
	vec2 uv = local_xz / (half_extent * 2.0f) + 0.5f;
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

	/* 高度图存 0..1，直接映射到 0..HeightScale */
	return mix(mix(h00, h10, frac.x), mix(h01, h11, frac.x), frac.y) * u_HeightScale;
}

/* 局部空间法线：邻居差分重建，step 是这个 LOD 的采样半径（米）。
 * 粗 LOD 用大 step，否则法线会跟着高度图的细节抖出锯齿。 */
vec3 TerrainNormalLocal(vec2 local_xz, float step)
{
	const float hl = TerrainHeight(local_xz - vec2(step, 0.0f));
	const float hr = TerrainHeight(local_xz + vec2(step, 0.0f));
	const float hd = TerrainHeight(local_xz - vec2(0.0f, step));
	const float hu = TerrainHeight(local_xz + vec2(0.0f, step));
	return normalize(vec3(hl - hr, 2.0f * step, hd - hu));
}

/* 世界空间法线：局部法线过逆转置，旋转与非均匀缩放都不会把坡向拧错 */
vec3 TerrainNormal(vec2 local_xz, float step)
{
	return normalize((u_TerrainNormalMatrix * vec4(TerrainNormalLocal(local_xz, step), 0.0f)).xyz);
}

/* 粗一档的表面：在 2×step 的网格上取样再线性插值，网格锚在地形局部原点。
 * 父节点的顶点正好落在这些格点上，所以边界处父子取到同一个高度，切换 LOD 不会跳。 */
float TerrainHeightCoarse(vec2 local_xz, float step)
{
	const vec2 local = local_xz / step;
	const vec2 cell = floor(local);
	const vec2 frac = local - cell;
	const vec2 base = cell * step;

	const float h00 = TerrainHeight(base);
	const float h10 = TerrainHeight(base + vec2(step, 0.0f));
	const float h01 = TerrainHeight(base + vec2(0.0f, step));
	const float h11 = TerrainHeight(base + vec2(step, step));
	return mix(mix(h00, h10, frac.x), mix(h01, h11, frac.x), frac.y);
}

/* 顶点阶段公共体：由 UV 与 patch 信息算世界位置，顺带把局部 xz 带出去给片元采样。
 * a_PatchInfo 的 xyz 是 patch 左下角（局部），w 是边长；a_PatchLod.y 是这个 LOD 的顶点间距（米）。
 * morph 把靠近 patch 边界的顶点拉向粗一档的表面：边界处（morph = 0）父子顶点逐点相等，
 * 内部（morph = 1）用细表面 —— LOD 切换是连续的，不再靠裙边遮。
 * 模板网格比 patch 多一圈顶点（UV 从 -1/32 到 33/32），那一圈仍拉成裙边兜底：
 * 四叉树没有 2:1 平衡，相邻节点可能差两档，morph 只保证差一档的情况。 */
vec3 TerrainVertexWorld(vec2 patch_uv, vec4 patch_info, vec4 patch_lod, out float morph, out vec2 local_xz)
{
	const float step = max(patch_lod.y, 0.05f);
	vec3 local = patch_info.xyz + vec3(patch_uv.x, 0.0f, patch_uv.y) * patch_info.w;
	local_xz = local.xz;

	const bool outside = patch_uv.x < 0.0f || patch_uv.x > 1.0f
		|| patch_uv.y < 0.0f || patch_uv.y > 1.0f;
	if (outside)
	{
		/* 裙边那一圈不参与 morph：直接按细表面拉下去 */
		morph = 1.0f;
		local.y = TerrainHeight(local.xz) - max(2.0f, patch_info.w * 0.03f);
		return (u_TerrainModel * vec4(local, 1.0f)).xyz;
	}

	const float height = TerrainHeight(local.xz);

	/* 逐条边 morph：只有挨着更粗邻居的那几条边才往父级表面收敛（patch_lod.z 的位掩码，
	 * 位序 1 = -X、2 = +X、4 = -Z、8 = +Z）。同层邻居两边都不收，细表面本来就一模一样 */
	const int side_mask = int(patch_lod.z + 0.5f);
	const float band = patch_info.w * 0.25f * u_TerrainMorphScale;
	if (side_mask == 0 || band <= step || patch_lod.x < 0.5f)
	{
		/* 没有更粗的邻居 / 关掉 morph（检查程序拿它做对照）：整片用细表面 */
		morph = 1.0f;
		local.y = height;
		return (u_TerrainModel * vec4(local, 1.0f)).xyz;
	}

	float edge_distance = 1.0e6f;
	if ((side_mask & 1) != 0)
		edge_distance = min(edge_distance, patch_uv.x * patch_info.w);
	if ((side_mask & 2) != 0)
		edge_distance = min(edge_distance, (1.0f - patch_uv.x) * patch_info.w);
	if ((side_mask & 4) != 0)
		edge_distance = min(edge_distance, patch_uv.y * patch_info.w);
	if ((side_mask & 8) != 0)
		edge_distance = min(edge_distance, (1.0f - patch_uv.y) * patch_info.w);

	const float t = clamp(edge_distance / band, 0.0f, 1.0f);
	morph = t * t * (3.0f - 2.0f * t);

	local.y = morph < 0.999f
		? mix(TerrainHeightCoarse(local.xz, step * 2.0f), height, morph)
		: height;
	return (u_TerrainModel * vec4(local, 1.0f)).xyz;
}

#endif // TERRAIN_COMMON_GLSL
