#ifndef MATERIAL_ALBEDO_GLSL
#define MATERIAL_ALBEDO_GLSL

/* 主贴图与它的 UV 变换：标准 lit 与无光照材质共用这一份声明，
 * 两类材质转进 G-Buffer 时参数名对得上 */
uniform sampler2D u_AlbedoTexture; // @group(Surface) @property

/* 主贴图 UV 缩放偏移（xy=缩放，zw=偏移）：所有材质贴图采样共用、缺省不变换 */
uniform vec4 u_AlbedoTilingOffset = vec4(1.0f, 1.0f, 0.0f, 0.0f); // @group(Surface) @property

vec2 ApplyAlbedoTiling(vec2 uv)
{
	return uv * u_AlbedoTilingOffset.xy + u_AlbedoTilingOffset.zw;
}

#endif // MATERIAL_ALBEDO_GLSL
