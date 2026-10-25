#ifndef DIRECT_LIGHT_GLSL
#define DIRECT_LIGHT_GLSL

#include "Uniforms.glsl"
#include "ShadowUtils.glsl"

/* 逐光源"笔"的合成开关：非 0 = 本笔合成环境项 / 自发光（逐笔合成时只能由第 0 笔累加）；
 * 0 = 只输出直接光。这个声明也是材质契约：渲染端按它判断 shader 是否走逐光源光照
 * （Material::SupportsDirectLighting） */
uniform int u_ComposeAmbientEmission = 1;

/* 平滑范围衰减：距离在 [0, range] 内按 (1 - d/range)^2 衰减，到 range 处归零。
 * 不用物理逆平方 —— 强度没有单位体系，逆平方在 d→0 时发散会把表面打爆 */
float RangeAttenuation(float distance, float range)
{
	float t = clamp(1.0f - distance / max(range, 1e-4f), 0.0f, 1.0f);
	return t * t;
}

/* 直接光评估（前向 / 延迟共用）：按光照 UBO 里当前光源的类型，求"指向光源"的向量、
 * 距离 / 锥角衰减和阴影因子。输出：attenuation（方向光恒为 1）、shadow（0 受光 / 1 遮挡） */
void EvaluateDirectLight(sampler2DArray shadow_map, vec3 world_pos, vec3 n,
	out vec3 l, out float attenuation, out float shadow)
{
	l = vec3(0.0f);
	attenuation = 1.0f;
	shadow = 0.0f;

	if (u_LightType == 0u)
	{
		/* 方向光：u_LightDir 为传播方向；无级联数据时不采样（单位阵会给出错误遮挡） */
		l = -normalize(u_LightDir);
		if (u_CascadeCount > 0u)
		{
			vec3 view_pos = (u_ViewMat * vec4(world_pos, 1.0f)).xyz;
			shadow = CalculateShadow(shadow_map, world_pos, view_pos, n, l);
		}
	}
	else if (u_LightType == 1u)
	{
		/* 点光：位置 + 范围衰减 */
		vec3 to_light = u_LightPos - world_pos;
		float distance = length(to_light);
		l = to_light / max(distance, 1e-5f);
		attenuation = RangeAttenuation(distance, u_PunctualLightParams.x);

		if (u_PunctualShadowParams.y > 0.5f)
			shadow = CalculatePointShadow(shadow_map, world_pos, n, u_LightPos);
	}
	else
	{
		/* 聚光（含未实现类型的兜底，按聚光语义处理）：点光衰减 × 锥角衰减 */
		vec3 to_light = u_LightPos - world_pos;
		float distance = length(to_light);
		l = to_light / max(distance, 1e-5f);
		attenuation = RangeAttenuation(distance, u_PunctualLightParams.x);

		/* 锥角衰减：轴向（cd=1）满强度，外锥（cd=cosOuter）外归零，内锥到外锥间平滑过渡 */
		float cd = dot(-l, normalize(u_LightDir));
		attenuation *= smoothstep(u_PunctualLightParams.z, u_PunctualLightParams.y, cd);

		if (u_PunctualShadowParams.y > 0.5f)
			shadow = CalculateSpotShadow(shadow_map, world_pos, n);
	}
}

#endif // DIRECT_LIGHT_GLSL
