#ifndef IBL_COMMON_GLSL
#define IBL_COMMON_GLSL

#include "../Builtin/LightCompose.glsl"

/* 反射探针 IBL 的运行期契约：支持 IBL 的 shader 都声明这组参数，由 ScenePass 按"最近已
 * 烘焙探针"逐绘制绑定（不写进材质）。u_UseIBL = 0 不参与着色；= 1 时必须已绑定三张贴图。
 * 烘焙半壁在同目录（Irradiance / Prefilter / BRDFLut + BakeCommon）。 */
layout(binding = 10) uniform samplerCube u_IrradianceMap;
layout(binding = 11) uniform samplerCube u_PrefilterMap;
layout(binding = 12) uniform sampler2D   u_BRDFLut;
/* 非 opaque uniform 需全 stage 唯一的 location：宿主 stage 的 0 被材质参数占用，这里固定为 1 */
layout(location = 1) uniform int u_UseIBL = 0;

/* 间接光：辐照度图（半球卷积）+ 粗糙度预滤波镜面（mip m 对应 roughness = m/(层数-1)），
 * 采样层级 = roughness × 最大 mip 序号；合成走公共库（与延迟侧同一份），怎么与直接光叠加由调用方决定 */
vec3 EvaluateIBL(vec3 N, vec3 V, float roughness, float metallic, vec3 albedo)
{
	roughness = clamp(roughness, 0.0f, 1.0f);
	const float ndv = max(dot(N, V), 0.0f);

	vec3 irradiance = texture(u_IrradianceMap, N).rgb;
	const float max_lod = log2(float(textureSize(u_PrefilterMap, 0).x));
	vec3 prefiltered = textureLod(u_PrefilterMap, reflect(-V, N), roughness * max_lod).rgb;
	vec2 brdf = texture(u_BRDFLut, vec2(ndv, roughness)).rg;

	return ComposeIBL(N, V, metallic, albedo, irradiance, prefiltered, brdf);
}

#endif // IBL_COMMON_GLSL
