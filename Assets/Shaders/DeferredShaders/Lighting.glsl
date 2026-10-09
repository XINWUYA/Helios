#type vertex
#version 410 core

struct SVextex2Frag
{
	vec2 TexCoord;
};

layout(location = 0) in vec4 a_Position;
layout(location = 0) out SVextex2Frag vert2frag;

void main()
{
	/* ZO + Reversed-Z：全屏三角形 z=1 即近平面（深度最大值），恒通过 GreaterEqual 比较 */
	gl_Position = a_Position;

	vert2frag.TexCoord = a_Position.xy * 0.5f + 0.5f;
}


#type fragment
#version 410 core

#include "../builtin/Uniforms.glsl"
#include "GBufferCommon.glsl"
#include "../builtin/Math.glsl"
#include "../builtin/BRDF.glsl"
#include "../builtin/ShadowUtils.glsl"
#include "../builtin/DirectLight.glsl"

struct SVextex2Frag
{
	vec2 TexCoord;
};

layout(location = 0) out vec4 OutFragColor;
layout(location = 0) in SVextex2Frag vert2frag;

/* 阴影贴图：GBuffer 占用 binding 0..5，这里取 9（与其它着色器同源，避开 GBuffer 绑定） */
layout(binding = 9) uniform sampler2DArray u_ShadowMap;

/* ---- 反射探针 IBL ----
 * 烘焙结果（辐照度 / 预滤波立方图）绑到固定采样器组：采样器不能动态索引，按"逐像素最近探针"
 * 选择、用 if 链取用；u_ProbeCount = 参与数（0 = 不做 IBL）。绑定 6..8 / 10..12 / 13 跟
 * GBuffer 0..5、阴影 9 错开。 */
layout(binding = 6)  uniform samplerCube u_IrradianceMap0;
layout(binding = 7)  uniform samplerCube u_IrradianceMap1;
layout(binding = 8)  uniform samplerCube u_IrradianceMap2;
layout(binding = 10) uniform samplerCube u_PrefilterMap0;
layout(binding = 11) uniform samplerCube u_PrefilterMap1;
layout(binding = 12) uniform samplerCube u_PrefilterMap2;
layout(binding = 13) uniform sampler2D   u_BRDFLut;

uniform int u_ProbeCount = 0;
uniform vec3 u_ProbePosition0 = vec3(0.0f);
uniform vec3 u_ProbePosition1 = vec3(0.0f);
uniform vec3 u_ProbePosition2 = vec3(0.0f);

/* 逐像素选择最近的探针（只在本帧参与集合内比较） */
int SelectIBLProbe(vec3 world_pos)
{
	int best = 0;
	float best_dist = distance(world_pos, u_ProbePosition0);
	if (u_ProbeCount > 1)
	{
		const float dist = distance(world_pos, u_ProbePosition1);
		if (dist < best_dist)
		{
			best_dist = dist;
			best = 1;
		}
	}
	if (u_ProbeCount > 2)
	{
		const float dist = distance(world_pos, u_ProbePosition2);
		if (dist < best_dist)
			best = 2;
	}
	return best;
}

/* 间接光：与 builtin/IBL.glsl 的 EvaluateIBL 同款语言（漫反射辐照度 + 预滤波镜面，
 * 采样层级 = roughness × 预滤波图的最大 mip 序号）。 */
vec3 EvaluateProbeIBL(int probe, vec3 N, vec3 V, float roughness, float metallic, vec3 albedo)
{
	roughness = clamp(roughness, 0.0f, 1.0f);
	const float ndv = max(dot(N, V), 0.0f);

	vec3 F = F_Schlick(ndv, metallic, albedo);
	vec3 kd = (vec3(1.0f) - F) * (1.0f - metallic);
	vec3 R = reflect(-V, N);

	vec3 irradiance;
	vec3 prefiltered;
	if (probe == 0)
	{
		irradiance = texture(u_IrradianceMap0, N).rgb;
		prefiltered = textureLod(u_PrefilterMap0, R, roughness * log2(float(textureSize(u_PrefilterMap0, 0).x))).rgb;
	}
	else if (probe == 1)
	{
		irradiance = texture(u_IrradianceMap1, N).rgb;
		prefiltered = textureLod(u_PrefilterMap1, R, roughness * log2(float(textureSize(u_PrefilterMap1, 0).x))).rgb;
	}
	else
	{
		irradiance = texture(u_IrradianceMap2, N).rgb;
		prefiltered = textureLod(u_PrefilterMap2, R, roughness * log2(float(textureSize(u_PrefilterMap2, 0).x))).rgb;
	}

	vec2 brdf = texture(u_BRDFLut, vec2(ndv, roughness)).rg;
	vec3 specular = prefiltered * (F * brdf.x + brdf.y);
	return kd * irradiance * albedo + specular;
}

/* 调试分流（Diffuse / Specular / Shadow / Indirect）在 DebugShaders/DebugLighting.glsl；
 * 须放在本文件的函数定义之后（它调用 SelectIBLProbe / EvaluateProbeIBL） */
#include "../DebugShaders/DebugLighting.glsl"

void main()
{
	SGBufferData gbuffer;
	CalculateGBuffer(gbuffer, vert2frag.TexCoord);

	vec3 world_pos = gbuffer.WorldPosition;
	vec3 n = normalize(gbuffer.WorldNormal);
	vec3 v = normalize(u_ViewPos - world_pos);

	/* 直接光评估（前向 / 延迟共用）：按本笔光源的类型求"指向光源"的向量、
	 * 衰减与阴影因子 */
	vec3 l;
	float attenuation;
	float shadow;
	EvaluateDirectLight(u_ShadowMap, world_pos, n, l, attenuation, shadow);

	/* 模型关闭「接受阴影」时阴影因子归零（延迟管线只能随 G-Buffer 拿到该标记） */
	shadow *= gbuffer.ReceiveShadow;

	/* 调试分流（Diffuse / Specular / Shadow / Indirect）：与正常着色共用同一条
	 * 最终颜色（lighting_result）—— 命中即写出并返回（alpha 统一补 1），未命中
	 * 在同一变量里继续合成（分量拆分见 ../DebugShaders/DebugLighting.glsl） */
	vec3 lighting_result;
	if (DebugLightingCompose(gbuffer, world_pos, n, v, l, attenuation, shadow, lighting_result))
	{
		OutFragColor = vec4(lighting_result, 1.0f);
		return;
	}

	vec3 direct = max(vec3(0.0f), BRDF(l, v, n, gbuffer.Metallic, gbuffer.Roughness, gbuffer.Albedo));
	direct *= u_ColorIntensity.rgb * u_ColorIntensity.a * attenuation;
	direct *= (1.0f - shadow * 0.8f);

	lighting_result = direct;

	/* 环境项 + 自发光只由负责合成的那一笔光照累加
	 * （见 builtin/DirectLight.glsl 的 u_ComposeAmbientEmission 注释） */
	if (u_ComposeAmbientEmission > 0)
	{
		/* 有反射探针（渲染通道绑定了本帧参与选择的探针子集）时，
		 * 环境项 = 逐像素选最近探针的 IBL；无探针时退回平面环境项 */
		if (u_ProbeCount > 0)
		{
			const int probe = SelectIBLProbe(world_pos);
			lighting_result += max(vec3(0.0f),
				EvaluateProbeIBL(probe, n, v, gbuffer.Roughness, gbuffer.Metallic, gbuffer.Albedo));
		}
		else
		{
			/* 平面环境项与 ForwardShaders 的无 IBL 回退同口径（PBRStandard / BuiltinLit 都带 0.3 系数） */
			lighting_result += max(vec3(0.0f), gbuffer.Ambient * gbuffer.Albedo * gbuffer.AO * 0.3f);
		}
		lighting_result += gbuffer.Emission;
	}

	OutFragColor = vec4(lighting_result, 1.0f);
}
