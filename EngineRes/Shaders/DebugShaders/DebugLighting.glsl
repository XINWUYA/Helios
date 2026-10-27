/* 延迟光照的调试分流（Diffuse / Specular / Indirect Diffuse / Indirect Specular / Probe）：
 * include 片段（没有自己的 stage / main），由 Lighting.glsl 在函数定义之后调用；
 * 注释里别写出 stage 声明关键字（ShaderCompiler 全文检索会误判）。Shadow 档要叠加多盏灯，
 * 在 DebugShaders/DebugShadow.glsl 里单独累加 */

#ifndef DEBUG_LIGHTING_GLSL
#define DEBUG_LIGHTING_GLSL

uniform int u_DebugView = 0;

const int kDebugDiffuse = 9;
const int kDebugSpecular = 10;
const int kDebugIndirectDiffuse = 12;
const int kDebugIndirectSpecular = 13;
const int kDebugProbe = 16;

/* 命中调试档位时把颜色写进 out_color 并返回 true（调用方负责输出）：
 * u_DebugView = 0（正常着色）返回 false；环境分量（Indirect 两档）只由第一笔光照合成 */
bool DebugLightingCompose(SGBufferData gbuffer, vec3 world_pos, vec3 n, vec3 v, vec3 l,
	float attenuation, float shadow, out vec3 out_color)
{
	if (u_DebugView == 0)
		return false;

	const vec3 light_factor = LightFactor(attenuation, shadow);

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

	if (u_DebugView == kDebugProbe)
	{
		/* 探针选择可视化：probe0 红 / probe1 绿 / probe2 蓝，混合区显示两色的渐变；
		 * 未启用混合或全部出盒的回落（距离最近）用暗色显示，免得和权重混合混淆 */
		const vec3 kProbeColors[3] = vec3[](vec3(1.0f, 0.1f, 0.1f),
			vec3(0.1f, 1.0f, 0.1f), vec3(0.1f, 0.4f, 1.0f));

		if (u_ProbeCount > 0)
		{
			const SProbeSelection selection = SelectIBLProbes(world_pos);
			vec3 color = kProbeColors[selection.Index0];
			if (selection.Weight1 > 0.0f)
				color = mix(color, kProbeColors[selection.Index1], selection.Weight1);
			out_color = selection.Weighted ? color : color * 0.35f;
		}
		else
		{
			out_color = vec3(0.0f);
		}
		return true;
	}

	/* Indirect Diffuse / Indirect Specular：环境项（IBL / 平面环境项）按分量拆开。
	 * 平面环境项只有漫反射，镜面分量在黑天空下本来就是零 */
	if (u_DebugView == kDebugIndirectDiffuse || u_DebugView == kDebugIndirectSpecular)
	{
		out_color = vec3(0.0f);
		if (u_ComposeAmbientEmission > 0)
		{
			/* 与正常着色的环境项同口径：阴影压的可见度一样要算进来 */
			const float ambient_visibility = AmbientShadowVisibility(n, l, shadow);
			if (u_ProbeCount > 0)
			{
				vec3 ibl_diffuse;
				vec3 ibl_specular;
				EvaluateProbeIBLComponents(SelectIBLProbes(world_pos), n, v, gbuffer.Roughness,
					gbuffer.Metallic, gbuffer.Albedo, ibl_diffuse, ibl_specular);
				out_color = max(vec3(0.0f), u_DebugView == kDebugIndirectDiffuse ? ibl_diffuse : ibl_specular)
					* ambient_visibility;
			}
			else if (u_DebugView == kDebugIndirectDiffuse)
			{
				/* 与正常路径的平面环境项走同一份合成（见 Builtin/LightCompose.glsl） */
				out_color = max(vec3(0.0f), ComposeFallbackAmbient(gbuffer.Ambient, gbuffer.Albedo, gbuffer.AO))
					* ambient_visibility;
			}
		}
		return true;
	}

	/* 光照阶段只收 Lighting 类档位，其余值留给正常着色 */
	return false;
}

#endif
