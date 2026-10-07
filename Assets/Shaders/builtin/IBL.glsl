#ifndef IBL_GLSL
#define IBL_GLSL

#include "BRDF.glsl"

/* 反射探针 IBL 的公共契约：支持 IBL 的 Shader 都声明这一组参数，由 ScenePass 按"最近已烘焙
 * 探针"逐绘制覆盖绑定（不写进材质对象）。u_UseIBL = 0 不参与着色；= 1 时必须已经绑定三张贴图。
 * 绑定点 10..12 在所有接入的 Shader 里一致。 */
layout(binding = 10) uniform samplerCube u_IrradianceMap;
layout(binding = 11) uniform samplerCube u_PrefilterMap;
layout(binding = 12) uniform sampler2D   u_BRDFLut;
uniform int u_UseIBL = 0;

/* 间接光（漫反射辐照度 + 粗糙度预滤波镜面）：辐照度图是半球卷积、预滤波图按粗糙度分级
 * （mip m 对应 roughness = m/(层数-1)），采样层级 = roughness × 最大 mip 序号（由尺寸反推）。
 * 直接光和间接光怎么合成由调用方决定。 */
vec3 EvaluateIBL(vec3 N, vec3 V, float roughness, float metallic, vec3 albedo)
{
	roughness = clamp(roughness, 0.0f, 1.0f);
	const float ndv = max(dot(N, V), 0.0f);

	/* 漫反射：辐照度 × 漫反射系数（F / kd 与直接光同一套 BRDF 语言） */
	vec3 F = F_Schlick(ndv, metallic, albedo);
	vec3 kd = (vec3(1.0f) - F) * (1.0f - metallic);
	vec3 diffuse = texture(u_IrradianceMap, N).rgb * albedo;

	/* 镜面反射：反射向量的预滤波采样 × 环境 BRDF 查找表 */
	const float max_lod = log2(float(textureSize(u_PrefilterMap, 0).x));
	vec3 R = reflect(-V, N);
	vec3 prefiltered = textureLod(u_PrefilterMap, R, roughness * max_lod).rgb;
	vec2 brdf = texture(u_BRDFLut, vec2(ndv, roughness)).rg;
	vec3 specular = prefiltered * (F * brdf.x + brdf.y);

	return kd * diffuse + specular;
}

#endif // IBL_GLSL
