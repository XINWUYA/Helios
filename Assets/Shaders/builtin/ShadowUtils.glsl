#ifndef SHADOW_UTILS_GLSL
#define SHADOW_UTILS_GLSL

#include "Uniforms.glsl"

/* 级联阴影计算（前向 / 延迟渲染通用）：依赖 LightUniformBuffer 里的 u_CascadeCount /
 * u_LightViewProjectionMat / u_CascadeSplits / u_CascadeShadowBias；采样器作参数传入、跟具体绑定
 * 点解耦。深度偏移 = 斜率自适应项 + CPU 折算的常数项。返回阴影因子：0 受光，1 被遮挡。 */
float CalculateShadow(sampler2DArray shadowMap, vec3 world_pos, vec3 view_pos)
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
	current_depth += u_ShadowBias;

	/* 3x3 PCF 软阴影 */
	float shadow = 0.0f;
	vec2 texel_size = 1.0f / vec2(textureSize(shadowMap, 0).xy);
	for (int x = -1; x <= 1; ++x)
	{
		for (int y = -1; y <= 1; ++y)
		{
			vec2 offset = vec2(x, y) * texel_size;
			float closest_depth = texture(shadowMap, vec3(shadow_uv + offset, float(cascade_id))).r;
			/* 遮挡判定：遮挡物比接收点更靠近光源 ⇒ 深度值更大 */
			shadow += current_depth < closest_depth ? 1.0f : 0.0f;
		}
	}

	return shadow / 9.0f;
}

#endif // SHADOW_UTILS_GLSL
