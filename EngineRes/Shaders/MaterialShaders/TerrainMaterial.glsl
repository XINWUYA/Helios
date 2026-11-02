#ifndef TERRAIN_MATERIAL_GLSL
#define TERRAIN_MATERIAL_GLSL

#include "TerrainCommon.glsl"

/* 地形外观：低处草色、陡坡岩色、高处砂色。
 * 可见着色（延迟 / 前向 / 调试）与虚拟纹理的页烘焙共用这一份 —— 改混色只改这里，
 * 页里烘出来的颜色才跟直接算的一模一样。 */
uniform vec3 u_GrassColor = vec3(0.26f, 0.36f, 0.16f);  // @property @type(color)
uniform vec3 u_RockColor = vec3(0.32f, 0.30f, 0.27f);   // @property @type(color)
uniform vec3 u_HighColor = vec3(0.55f, 0.47f, 0.34f);   // @property @type(color)
uniform float u_Roughness = 0.85f;                       // @property @range(0.05, 1.0)
uniform float u_RockSlope = 0.45f;                       // @property @range(0.0, 1.0)

struct STerrainSurface
{
	vec3 Albedo;
	float Roughness;
	vec3 Normal;
};

STerrainSurface EvaluateTerrainSurface(vec2 world_xz, float step)
{
	STerrainSurface surface;
	surface.Normal = TerrainNormal(world_xz, step);

	/* 坡度 = 1 - N.y：陡的地方露岩，缓的地方长草 */
	const float slope = clamp(1.0f - surface.Normal.y, 0.0f, 1.0f);
	surface.Albedo = mix(u_GrassColor, u_RockColor, smoothstep(u_RockSlope * 0.5f, u_RockSlope, slope));

	/* 高处混一点砂色，远山才有层次 */
	const float height_t = clamp(TerrainHeight(world_xz) / max(u_HeightScale, 1.0f), 0.0f, 1.0f);
	surface.Albedo = mix(surface.Albedo, u_HighColor, smoothstep(0.55f, 0.9f, height_t) * 0.65f);

	surface.Roughness = u_Roughness;
	return surface;
}

#endif // TERRAIN_MATERIAL_GLSL
