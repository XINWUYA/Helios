#ifndef TERRAIN_MATERIAL_GLSL
#define TERRAIN_MATERIAL_GLSL

#include "TerrainCommon.glsl"

/* 地表四层：草 / 岩 / 砂 / 雪。每层 = 颜色 × 贴图，贴图缺省是白图 ——
 * 不配贴图时就是平涂色，与最早的三色版观感一致。
 * 贴图按局部 xz 平铺（Tiling = 一张贴图铺多少米），旋转地形纹路跟着山走。
 * 可见着色（延迟 / 前向 / 调试）与页烘焙共用这一份，改混色只改这里。 */
uniform sampler2D u_Layer0Albedo;                          // @group(Grass layer) @property(Texture)
uniform vec3 u_Layer0Color = vec3(0.26f, 0.36f, 0.16f);    // @group(Grass layer) @property(Color) @type(color)
uniform float u_Layer0Tiling = 8.0f;                       // @group(Grass layer) @property(Tile size) @range(1.0, 64.0)
uniform sampler2D u_Layer1Albedo;                          // @group(Rock layer) @property(Texture)
uniform vec3 u_Layer1Color = vec3(0.32f, 0.30f, 0.27f);    // @group(Rock layer) @property(Color) @type(color)
uniform float u_Layer1Tiling = 12.0f;                      // @group(Rock layer) @property(Tile size) @range(1.0, 64.0)
uniform sampler2D u_Layer2Albedo;                          // @group(Sand layer) @property(Texture)
uniform vec3 u_Layer2Color = vec3(0.55f, 0.47f, 0.34f);    // @group(Sand layer) @property(Color) @type(color)
uniform float u_Layer2Tiling = 10.0f;                      // @group(Sand layer) @property(Tile size) @range(1.0, 64.0)
uniform sampler2D u_Layer3Albedo;                          // @group(Snow layer) @property(Texture)
uniform vec3 u_Layer3Color = vec3(0.86f, 0.88f, 0.92f);    // @group(Snow layer) @property(Color) @type(color)
uniform float u_Layer3Tiling = 16.0f;                      // @group(Snow layer) @property(Tile size) @range(1.0, 64.0)

uniform float u_Roughness = 0.85f;                         // @group(Surface) @property(Roughness) @range(0.05, 1.0)
/* 岩层起始坡度（1 - N.y）：超过它的坡面开始露岩 */
uniform float u_RockSlope = 0.45f;                         // @group(Layer blending) @property(Rock threshold) @range(0.0, 1.0)
/* 砂层上限：低于这个高度比例铺砂，往上 8% 内淡出 */
uniform float u_SandHeight = 0.06f;                        // @group(Layer blending) @property(Sand height) @range(0.0, 1.0)
/* 雪线：高于这个高度比例开始积雪，往上 18% 内铺满 */
uniform float u_SnowHeight = 0.62f;                        // @group(Layer blending) @property(Snow line) @range(0.0, 1.0)

/* 法线差分的固定步长（米）：由高度图纹素跨度推，所有 LOD 用同一个。
 * 跟着 patch 走的话，粗 patch 与细 patch 交界的坡度会算得不同，混色切出一条直带 */
uniform float u_TerrainNormalStep = 1.0f;

struct STerrainSurface
{
	vec3 Albedo;
	float Roughness;
	vec3 Normal;
};

vec3 TerrainLayer(vec2 local_xz, sampler2D map, vec3 color, float tiling)
{
	return color * texture(map, local_xz / max(tiling, 0.01f)).rgb;
}

STerrainSurface EvaluateTerrainSurface(vec2 local_xz)
{
	STerrainSurface surface;
	const float step = max(u_TerrainNormalStep, 0.01f);
	const vec3 local_normal = TerrainNormalLocal(local_xz, step);
	surface.Normal = TerrainNormal(local_xz, step);

	/* 四层都采一遍，权重为 0 的层也是同一份贴图操作 —— 放进分支里
	 * 导数会在发散处算飞，贴图 mip 跟着抖。直接算的路径 4 次采样，
	 * 走虚拟纹理时页里已经混好，只剩 1 次 */
	const vec3 grass = TerrainLayer(local_xz, u_Layer0Albedo, u_Layer0Color, u_Layer0Tiling);
	const vec3 rock = TerrainLayer(local_xz, u_Layer1Albedo, u_Layer1Color, u_Layer1Tiling);
	const vec3 sand = TerrainLayer(local_xz, u_Layer2Albedo, u_Layer2Color, u_Layer2Tiling);
	const vec3 snow = TerrainLayer(local_xz, u_Layer3Albedo, u_Layer3Color, u_Layer3Tiling);

	/* 坡度 = 1 - N.y：陡的地方露岩，缓的地方长草。按局部法线算，
	 * 旋转地形只改朝向，不会把整片坡面判成岩色 */
	const float slope = clamp(1.0f - local_normal.y, 0.0f, 1.0f);
	const float height_t = clamp(TerrainHeight(local_xz) / max(u_HeightScale, 1.0f), 0.0f, 1.0f);

	const float rock_weight = smoothstep(u_RockSlope * 0.5f,
		max(u_RockSlope, u_RockSlope * 0.5f + 1e-6f), slope);
	const float sand_weight = 1.0f - smoothstep(u_SandHeight, u_SandHeight + 0.08f, height_t);
	/* 陡壁挂不住雪：坡度越大雪越薄 */
	const float snow_weight = smoothstep(u_SnowHeight, u_SnowHeight + 0.18f, height_t)
		* (1.0f - 0.6f * smoothstep(0.25f, 0.7f, slope));

	/* 覆盖顺序：砂垫底处的低地，岩盖陡坡，雪压山顶 */
	vec3 albedo = mix(grass, sand, sand_weight);
	albedo = mix(albedo, rock, rock_weight);
	albedo = mix(albedo, snow, snow_weight);

	surface.Albedo = albedo;
	surface.Roughness = u_Roughness;
	return surface;
}

#endif // TERRAIN_MATERIAL_GLSL
