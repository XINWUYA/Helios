#ifndef SHADOW_UTILS_GLSL
#define SHADOW_UTILS_GLSL

#include "Uniforms.glsl"

/* 阴影图数组：由本库声明，用阴影的宿主别再各自声明；渲染端按名字查槽位绑定。 */
uniform sampler2DArray u_ShadowMap;

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
			/* 遮挡判定：遮挡物更靠近光源 ⇒ 深度值更大（Reversed-Z） */
			shadow += current_depth < closest_depth ? 1.0f : 0.0f;
		}
	}

	return shadow / 9.0f;
}

/* 该级联单个纹素对应的世界尺寸：正交投影下 light_vp 的 x 缩放 = 2 / 正交宽度，
 * 故单纹素世界尺寸 = 2 / (m00 * 阴影图边长)。 */
float CascadeTexelWorldSize(int cascade_id, int shadow_size)
{
	const float m00 = abs(u_LightViewProjectionMat[cascade_id][0][0]);
	return 2.0f / max(m00 * float(shadow_size), 1e-6f);
}

/* 单层级联采样：返回遮挡比例（0 受光，1 全遮挡）；inside 表示该点是否落在该级联的正交盒内。
 * world_pos 已做过法线偏移，normal 已归一化。 */
float SampleCascadeShadow(sampler2DArray shadowMap, int cascade_id, vec3 world_pos, vec3 normal, vec3 light_dir, out bool inside)
{
	vec4 light_space_pos = u_LightViewProjectionMat[cascade_id] * vec4(world_pos, 1.0f);
	light_space_pos /= light_space_pos.w;

	/* ZO + Reversed-Z：光照空间 z 已在 [0,1] 且越近光源值越大，xy 重映射到 [0,1] 即可 */
	vec2 shadow_uv = light_space_pos.xy * 0.5f + 0.5f;
	float current_depth = light_space_pos.z;

	inside = current_depth >= 0.0f && current_depth <= 1.0f
		&& shadow_uv.x >= 0.0f && shadow_uv.x <= 1.0f
		&& shadow_uv.y >= 0.0f && shadow_uv.y <= 1.0f;
	if (!inside)
		return 0.0f;

	/* Reversed-Z 下自阴影偏移方向反转：接收点深度加 bias 后更靠近光源（值更大） */
	const float ndl = clamp(dot(normal, light_dir), 0.0f, 1.0f);
	const float slope_tan = sqrt(max(1.0f - ndl * ndl, 0.0f)) / max(ndl, 1e-3f);
	const float slope_bias = min(0.02f, 1.5f * slope_tan / float(textureSize(shadowMap, 0).x));
	current_depth += slope_bias + u_CascadeShadowBias[cascade_id];

	return SampleShadowPCF(shadowMap, shadow_uv, float(cascade_id), current_depth);
}

/* 级联阴影计算（前向 / 延迟通用）：依赖 LightUniformBuffer 里的级联数 / 矩阵 / 分割距离 /
 * 逐级联偏移与过渡带参数。返回阴影因子：0 受光，1 被遮挡。 */
float CalculateShadow(sampler2DArray shadowMap, vec3 world_pos, vec3 view_pos, vec3 normal, vec3 light_dir)
{
	const int shadow_size = textureSize(shadowMap, 0).x;
	const vec3 n = normalize(normal);

	int cascade_id = 0;
	float view_depth = abs(view_pos.z);
	for (int i = 0; i < int(u_CascadeCount) - 1; ++i)
	{
		if (view_depth > u_CascadeSplits[i])
			cascade_id = i + 1;
	}

	/* 法线偏移：沿法线抬起若干纹素的世界距离，压住斜率偏移兜不住的 acne */
	const vec3 sample_pos = world_pos + n * (u_NormalBias * CascadeTexelWorldSize(cascade_id, shadow_size));
	bool inside = false;
	float factor = SampleCascadeShadow(shadowMap, cascade_id, sample_pos, n, light_dir, inside);

	/* 级联过渡带：split 之前一段距离里连下一层一起采，按距离插值，抹平两层分辨率差造成的硬边。
	 * 下一层没盖住这个点时（正交盒外）保持本层结果。 */
	if (cascade_id + 1 < int(u_CascadeCount) && u_CascadeBlend > 0.0f)
	{
		const float split = u_CascadeSplits[cascade_id];
		const float prev_split = (cascade_id > 0) ? u_CascadeSplits[cascade_id - 1] : 0.0f;
		const float band = max(split - prev_split, 1e-4f) * u_CascadeBlend;
		if (view_depth > split - band)
		{
			const int next_id = cascade_id + 1;
			const vec3 next_pos = world_pos + n * (u_NormalBias * CascadeTexelWorldSize(next_id, shadow_size));
			bool next_inside = false;
			const float next_factor = SampleCascadeShadow(shadowMap, next_id, next_pos, n, light_dir, next_inside);
			if (next_inside)
			{
				const float t = clamp((view_depth - (split - band)) / band, 0.0f, 1.0f);
				factor = mix(factor, next_factor, t);
			}
		}
	}

	return factor;
}

/* 点光 / 聚光阴影的 z 空间深度偏移：ShadowBias 是世界单位，但比较发生在投影后的 z 空间，
 * 直接加数量级对不上 —— 换算成 z 斜率（世界偏移项 + 防掠射 acne 的自适应项）。 */
float PunctualShadowZOffset(sampler2DArray shadowMap, vec3 light_to_frag, vec3 n)
{
	const float near_plane = u_PunctualLightParams.w;
	const float far_plane = u_PunctualShadowParams.z;
	const float distance = length(light_to_frag);
	const float distance_sq = max(distance * distance, 1e-6f);

	/* 常数项：世界偏移按 d 处的透视 z 斜率折算 */
	const float constant_z = u_ShadowBias * near_plane * far_plane
		/ max((far_plane - near_plane) * distance_sq, 1e-6f);

	/* 斜率自适应项：1.5 ×（单纹素世界尺寸）× tanθ × |dz/dd|。
	 * ndl 用「指向光源」方向 —— light_to_frag 是光源指向片元，取负才是 */
	const float ndl = clamp(dot(normalize(n), -light_to_frag / max(distance, 1e-5f)), 0.0f, 1.0f);
	const float slope_tan = sqrt(max(1.0f - ndl * ndl, 0.0f)) / max(ndl, 1e-3f);
	const float slope_z = 3.0f * near_plane * far_plane * slope_tan
		/ max((far_plane - near_plane) * distance * float(textureSize(shadowMap, 0).x), 1e-6f);

	return constant_z + slope_z;
}

/* 点光立方体阴影（前向 / 延迟共用）。面选择与 CPU 端 GetPunctualLightViewMatrix 的顺序一致
 * （+X, -X, +Y, -Y, +Z, -Z）。返回阴影因子：0 受光，1 被遮挡。 */
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

/* 聚光单面阴影（前向 / 延迟共用）：投影视锥就是光锥，uv 来自该光源单一的视图投影矩阵；
 * 光锥外 / 光源背后的片元光照本来就为 0，采样越界没影响。 */
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
