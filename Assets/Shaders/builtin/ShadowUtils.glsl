#ifndef SHADOW_UTILS_GLSL
#define SHADOW_UTILS_GLSL

#include "Uniforms.glsl"

/* 3x3 PCF 软阴影采样（级联 / 点光 / 聚光共用）；current_depth 是接收点深度（bias 已含），
 * shadow_uv = 光照空间 xy 重映射到 [0,1]。返回遮挡比例：0 受光，1 被遮挡。 */
float SampleShadowPCF(sampler2DArray shadowMap, vec2 shadow_uv, float layer, float current_depth)
{
	float shadow = 0.0f;
	vec2 texel_size = 1.0f / vec2(textureSize(shadowMap, 0).xy);
	for (int x = -1; x <= 1; ++x)
	{
		for (int y = -1; y <= 1; ++y)
		{
			vec2 offset = vec2(x, y) * texel_size;
			float closest_depth = texture(shadowMap, vec3(shadow_uv + offset, layer)).r;
			/* 遮挡判定：遮挡物比接收点更靠近光源 ⇒ 深度值更大（Reversed-Z） */
			shadow += current_depth < closest_depth ? 1.0f : 0.0f;
		}
	}

	return shadow / 9.0f;
}

/* 级联阴影计算（前向 / 延迟渲染通用）：依赖 LightUniformBuffer 里的 u_CascadeCount /
 * u_LightViewProjectionMat / u_CascadeSplits / u_CascadeShadowBias；采样器作参数传入、跟具体绑定
 * 点解耦。深度偏移 = 斜率自适应项 + CPU 折算的常数项。返回阴影因子：0 受光，1 被遮挡。 */
float CalculateShadow(sampler2DArray shadowMap, vec3 world_pos, vec3 view_pos, vec3 normal, vec3 light_dir)
{
	int cascade_id = 0;
	float view_depth = abs(view_pos.z);

	for (int i = 0; i < int(u_CascadeCount) - 1; ++i)
	{
		if (view_depth > u_CascadeSplits[i])
			cascade_id = i + 1;
	}

	mat4 light_vp_mat = u_LightViewProjectionMat[cascade_id];

	vec4 light_space_pos = light_vp_mat * vec4(world_pos, 1.0f);
	light_space_pos /= light_space_pos.w;

	/* ZO + Reversed-Z：光照空间 z 已在 [0,1] 且越靠近光源值越大，
	 * 仅 xy 需要从 [-1,1] 重映射到 [0,1]，z 直接使用 */
	vec2 shadow_uv = light_space_pos.xy * 0.5f + 0.5f;
	float current_depth = light_space_pos.z;

	if (current_depth > 1.0f || current_depth < 0.0f)
		return 0.0f;

	/* Reversed-Z 下自阴影偏移方向反转：接收点深度加 bias 后更靠近光源（值更大） */
	float ndl = clamp(dot(normalize(normal), light_dir), 0.0f, 1.0f);
	float slope_tan = sqrt(max(1.0f - ndl * ndl, 0.0f)) / max(ndl, 1e-3f);
	float slope_bias = min(0.02f, 1.5f * slope_tan / float(textureSize(shadowMap, 0).x));
	float constant_bias = u_CascadeShadowBias[cascade_id];
	current_depth += slope_bias + constant_bias;

	return SampleShadowPCF(shadowMap, shadow_uv, float(cascade_id), current_depth);
}

/* 点光 / 聚光阴影的 z 空间深度偏移：ShadowBias 是世界单位，但比较发生在投影后的 z 空间
 * （z(d) = nf(d+f)/((f-n)d)，斜率 nf/((f-n)d²)）—— 直接把世界单位加上去数量级对不上。偏移 = 世界
 * 偏移 × z 斜率 + 斜率自适应项（防掠射面 acne；纹素尺寸按 d/size 估）。 */
float PunctualShadowZOffset(sampler2DArray shadowMap, vec3 light_to_frag, vec3 n)
{
	const float near_plane = u_PunctualLightParams.w;
	const float far_plane = u_PunctualShadowParams.z;
	const float distance = length(light_to_frag);
	const float distance_sq = max(distance * distance, 1e-6f);

	/* 常数项：世界偏移按 d 处的透视 z 斜率折算 */
	const float constant_z = u_ShadowBias * near_plane * far_plane
		/ max((far_plane - near_plane) * distance_sq, 1e-6f);

	/* 斜率自适应项：1.5 ×（单纹素世界尺寸）× tanθ × |dz/dd| */
	const float ndl = clamp(dot(normalize(n), light_to_frag / max(distance, 1e-5f)), 0.0f, 1.0f);
	const float slope_tan = sqrt(max(1.0f - ndl * ndl, 0.0f)) / max(ndl, 1e-3f);
	const float slope_z = 3.0f * near_plane * far_plane * slope_tan
		/ max((far_plane - near_plane) * distance * float(textureSize(shadowMap, 0).x), 1e-6f);

	return constant_z + slope_z;
}

/* 点光立方体阴影（前向 / 延迟共用）。依赖 LightUniformBuffer 里的 u_PunctualShadowMat /
 * u_PunctualShadowParams / u_ShadowBias（逐光源填充）；面选择跟 CPU 端
 * GetPunctualLightViewMatrix 的顺序一致（+X, -X, +Y, -Y, +Z, -Z）。
 * 返回阴影因子：0 受光，1 被遮挡。 */
float CalculatePointShadow(sampler2DArray shadowMap, vec3 world_pos, vec3 n, vec3 light_pos)
{
	vec3 light_to_frag = world_pos - light_pos;

	/* 超出光源范围（阴影远平面）的部分不产生阴影，避免范围外的"幽灵遮挡" */
	float far_plane = u_PunctualShadowParams.z;
	if (dot(light_to_frag, light_to_frag) >= far_plane * far_plane)
		return 0.0f;

	/* 立方体面选择：主轴方向决定面 */
	vec3 abs_dir = abs(light_to_frag);
	int face = (light_to_frag.x > 0.0f) ? 0 : 1;
	float major = abs_dir.x;
	if (abs_dir.y > major)
	{
		major = abs_dir.y;
		face = (light_to_frag.y > 0.0f) ? 2 : 3;
	}
	if (abs_dir.z > major)
	{
		face = (light_to_frag.z > 0.0f) ? 4 : 5;
	}

	vec4 light_space_pos = u_PunctualShadowMat[face] * vec4(world_pos, 1.0f);
	light_space_pos /= light_space_pos.w;

	vec2 shadow_uv = light_space_pos.xy * 0.5f + 0.5f;
	float current_depth = light_space_pos.z;

	if (current_depth > 1.0f || current_depth < 0.0f)
		return 0.0f;

	current_depth += PunctualShadowZOffset(shadowMap, light_to_frag, n);

	return SampleShadowPCF(shadowMap, shadow_uv, u_PunctualShadowParams.x + float(face), current_depth);
}

/* 聚光单面阴影（前向 / 延迟共用）。投影视锥就是光锥（按外锥角构造透视矩阵），uv 来自该
 * 光源单一的视图投影矩阵；光锥外 / 光源背后的片元光照本来就为 0，采样越界没影响。
 * 返回阴影因子：0 受光，1 被遮挡。 */
float CalculateSpotShadow(sampler2DArray shadowMap, vec3 world_pos, vec3 n)
{
	vec4 light_space_pos = u_PunctualShadowMat[0] * vec4(world_pos, 1.0f);
	light_space_pos /= light_space_pos.w;

	vec2 shadow_uv = light_space_pos.xy * 0.5f + 0.5f;
	float current_depth = light_space_pos.z;

	if (current_depth > 1.0f || current_depth < 0.0f)
		return 0.0f;

	current_depth += PunctualShadowZOffset(shadowMap, world_pos - u_LightPos, n);

	return SampleShadowPCF(shadowMap, shadow_uv, u_PunctualShadowParams.x, current_depth);
}

#endif // SHADOW_UTILS_GLSL
