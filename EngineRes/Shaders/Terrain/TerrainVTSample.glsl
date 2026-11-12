#ifndef TERRAIN_VT_SAMPLE_GLSL
#define TERRAIN_VT_SAMPLE_GLSL

/* 虚拟纹理采样：页表给层号，0 表示这一页没驻留（回退到逐像素直接算）。
 * 页宽 64 米与 C++ 的 TerrainVirtualTexture::kPageWorldSize 一致，改一处要同步另一处。 */
uniform sampler2D u_VTPageTable;
uniform sampler2DArray u_VTPageAtlas;
/* x/y = 页表尺寸；0 表示没开虚拟纹理（采样器仍要绑，Metal 缺绑定会断言） */
uniform vec2 u_VTGrid = vec2(0.0f);

/* 远景底图：整块地形一张低分辨率颜色图。xy 是左下角、z 是边长、w 是每纹素多少米，
 * z = 0 表示没有底图（采样器仍要绑，缺绑定会断言） */
uniform sampler2D u_Basemap;
uniform vec4 u_BasemapRect = vec4(0.0f);

bool TerrainVirtualTextureLookup(vec2 world_xz, out vec4 out_albedo)
{
	out_albedo = vec4(0.0f);
	if (u_VTGrid.x < 1.0f)
		return false;

	const vec2 min_corner = u_TerrainOrigin.xz - u_TerrainWorldExtent;
	const vec2 local = (world_xz - min_corner) / 64.0f;
	const vec2 cell = floor(local);

	const ivec2 page = ivec2(cell);
	if (page.x < 0 || page.y < 0 || page.x >= int(u_VTGrid.x) || page.y >= int(u_VTGrid.y))
		return false;

	const float layer = texelFetch(u_VTPageTable, page, 0).r;
	if (layer < 0.5f)
		return false;

	/* 页池只有一级 mip，直接取第 0 级：页内 uv 在页边界会从 0.99 跳到 0.01，
	 * 自动导数会把 LOD 算飞；实测 iOS 真机上 textureGrad 采数组会算出发散地址，
	 * 整个命令缓冲报 GPU 页错误。以后页池加 mip 再回来接显式梯度 */
	const vec2 uv = local - cell;
	out_albedo = textureLod(u_VTPageAtlas, vec3(uv, layer - 1.0f), 0.0);
	return true;
}

/* 远景底图：页没驻留、而像素在地面上的足迹又比底图一个纹素还大时采它。
 * 远处逐像素重算混色最贵，又看不出细节，底图正好够用 */
bool TerrainBasemapLookup(vec2 world_xz, float footprint, out vec4 out_albedo)
{
	out_albedo = vec4(0.0f);
	if (u_BasemapRect.z <= 0.0f || footprint < u_BasemapRect.w)
		return false;

	const vec2 uv = (world_xz - u_BasemapRect.xy) / u_BasemapRect.z;
	if (uv.x < 0.0f || uv.y < 0.0f || uv.x > 1.0f || uv.y > 1.0f)
		return false;

	out_albedo = texture(u_Basemap, uv);
	return true;
}

#endif // TERRAIN_VT_SAMPLE_GLSL
