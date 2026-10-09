/* 延迟光照的调试分流（Lighting 类档位）：Diffuse / Specular / Shadow / Indirect。include 片段
 * （没有 stage / main），由 Lighting.glsl 在"光照分解"处调用。注意：注释里别写出引擎的
 * stage 声明关键字（ShaderCompiler 全文检索会误判）；要在 u_ComposeAmbientEmission / IBL /
 * BRDF 定义之后 include。 */

#ifndef DEBUG_LIGHTING_GLSL
#define DEBUG_LIGHTING_GLSL

uniform int u_DebugView = 0;
uniform int u_DebugShadowCompose = 0;

const int kDebugDiffuse = 9;
const int kDebugSpecular = 10;
const int kDebugShadow = 11;
const int kDebugIndirect = 12;

/* 命中调试档位时把颜色写进 out_color 并返回 true（调用方 OutFragColor = vec4(out_color, 1.0f)）。
 * 正常着色（u_DebugView = 0）返回 false、不受影响；直接光分量跟正常路径一样乘光照参数，
 * 环境分量（Indirect）只由第一笔光照合成。 */
bool DebugLightingCompose(SGBufferData gbuffer, vec3 world_pos, vec3 n, vec3 v, vec3 l,
	float attenuation, float shadow, out vec3 out_color)
{
	if (u_DebugView == 0)
		return false;

	const vec3 light_factor = u_ColorIntensity.rgb * u_ColorIntensity.a * attenuation * (1.0f - shadow * 0.8f);

	if (u_DebugView == kDebugDiffuse)
	{
		out_color = max(vec3(0.0f), BRDFDiffuse(l, v, n, gbuffer.Metallic, gbuffer.Roughness, gbuffer.Albedo)) * light_factor;
		return true;
	}
	if (u_DebugView == kDebugSpecular)
	{
		out_color = max(vec3(0.0f), BRDFSpecular(l, v, n, gbuffer.Metallic, gbuffer.Roughness, gbuffer.Albedo)) * light_factor;
		return true;
	}
	if (u_DebugView == kDebugShadow)
	{
		/* 可见性因子：1 = 受光、0 = 被遮挡 */
		out_color = vec3(u_DebugShadowCompose != 0 ? 1.0f - shadow : 0.0f);
		return true;
	}

	/* kDebugIndirect：环境项（IBL / 平面环境项） */
	vec3 indirect = vec3(0.0f);
	if (u_ComposeAmbientEmission > 0)
	{
		if (u_ProbeCount > 0)
		{
			const int probe = SelectIBLProbe(world_pos);
			indirect = max(vec3(0.0f),
				EvaluateProbeIBL(probe, n, v, gbuffer.Roughness, gbuffer.Metallic, gbuffer.Albedo));
		}
		else
		{
			/* 与正常路径的平面环境项同口径（含 0.3 系数，见 Lighting.glsl 合成段） */
			indirect = max(vec3(0.0f), gbuffer.Ambient * gbuffer.Albedo * gbuffer.AO * 0.3f);
		}
	}
	out_color = indirect;
	return true;
}

#endif
