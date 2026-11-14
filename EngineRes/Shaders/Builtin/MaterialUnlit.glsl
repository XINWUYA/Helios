#ifndef MATERIAL_UNLIT_GLSL
#define MATERIAL_UNLIT_GLSL

#include "Math.glsl"
#include "MaterialAlbedo.glsl"

/* 无光照材质：颜色 = 主贴图（转线性）× 顶点色 × u_Color，不受光源、阴影与环境光影响。
 * 前向 MaterialShaders/Unlit.glsl 与延迟 GBuffer 的无光照变体共用这一份。
 * GBuffer.glsl 的 u_Unlit 缺省 0，@showif 让这一行只在无光照材质的面板上出现 */
uniform vec4 u_Color = vec4(1.0f); // @group(Surface) @property @type(color) @showif(u_Unlit, 1)

vec4 CalculateUnlitColor(vec2 uv, vec4 vertex_color)
{
	const vec4 albedo = texture(u_AlbedoTexture, ApplyAlbedoTiling(uv));
	return vec4(SrgbToLinear(albedo.rgb), albedo.a) * vertex_color * u_Color;
}

#endif // MATERIAL_UNLIT_GLSL
