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
	/* 全屏三角形：z=1 即近平面（Reversed-Z），恒通过 GreaterEqual 深度比较 */
	gl_Position = a_Position;

	vert2frag.TexCoord = a_Position.xy * 0.5f + 0.5f;
}


#type fragment
#version 410 core

#include "../Builtin/Uniforms.glsl"
#include "GBufferCommon.glsl"
#include "../Builtin/Math.glsl"
#include "../Builtin/BRDF.glsl"
#include "../Builtin/ShadowUtils.glsl"
#include "../Builtin/DirectLight.glsl"
#include "../Builtin/LightCompose.glsl"

struct SVextex2Frag
{
	vec2 TexCoord;
};

layout(location = 0) out vec4 OutFragColor;
layout(location = 0) in SVextex2Frag vert2frag;

/* 采样器绑定按功能分段：GBuffer 0..5、探针漫反射 7..9、探针镜面 10..12、BRDF LUT 13；
 * 阴影图 15（跨 Pass 统一收尾槽，声明在 ShadowUtils.glsl） */

/* 反射探针 IBL：烘焙结果绑固定采样器组 —— 采样器不能动态索引，按"逐像素选择"用
 * if 链取用；u_ProbeCount = 0 不做 IBL */
layout(binding = 7)  uniform samplerCube u_IrradianceMap0;
layout(binding = 8)  uniform samplerCube u_IrradianceMap1;
layout(binding = 9)  uniform samplerCube u_IrradianceMap2;
layout(binding = 10) uniform samplerCube u_PrefilterMap0;
layout(binding = 11) uniform samplerCube u_PrefilterMap1;
layout(binding = 12) uniform samplerCube u_PrefilterMap2;
layout(binding = 13) uniform sampler2D   u_BRDFLut;

/* 非 opaque uniform 必须显式 location（Windows 走 shaderc）：探针参数占 0..3，
 * 4 = u_ComposeAmbientEmission（DirectLight.glsl）、5/6 = 调试分流（DebugLighting.glsl）、
 * 7..9 = 探针盒体、10 = 混合开关 */
layout(location = 0) uniform int  u_ProbeCount = 0;
layout(location = 1) uniform vec3 u_ProbePosition0 = vec3(0.0f);
layout(location = 2) uniform vec3 u_ProbePosition1 = vec3(0.0f);
layout(location = 3) uniform vec3 u_ProbePosition2 = vec3(0.0f);
/* 盒体参数：xyz = 半尺寸、w = 混合距离（0 = 该探针不做过渡） */
layout(location = 7) uniform vec4 u_ProbeExtent0 = vec4(0.0f);
layout(location = 8) uniform vec4 u_ProbeExtent1 = vec4(0.0f);
layout(location = 9) uniform vec4 u_ProbeExtent2 = vec4(0.0f);
/* 混合开关：0 = 没有探针配过渡，走旧的"距离最近"路径 */
layout(location = 10) uniform int u_ProbeBlendEnabled = 0;

/* 探针选择的完整结果：混合路径输出 top2 的归一化权重 */
struct SProbeSelection
{
	int Index0;
	int Index1;
	float Weight0;   /* 归一化后 top1 权重（纯选择时 = 1） */
	float Weight1;   /* 归一化后 top2 权重（0 = 不混合） */
	bool Weighted;   /* false = 距离最近路径（混合路径下全部出盒时也回落这里） */
};

/* 盒内深度：到最近盒面的余量（出盒为负） */
float ProbeBoxDepth(vec3 world_pos, vec4 extent, vec3 probe_pos)
{
	const vec3 delta = abs(world_pos - probe_pos);
	return min(min(extent.x - delta.x, extent.y - delta.y), extent.z - delta.z);
}

/* 盒体权重：盒内深度过 blend 拉满（越靠盒边越低）；blend = 0 时盒内即满权重（阶跃） */
float ProbeBoxWeight(vec3 world_pos, int probe)
{
	vec4 extent = u_ProbeExtent0;
	vec3 probe_pos = u_ProbePosition0;
	if (probe == 1)
	{
		extent = u_ProbeExtent1;
		probe_pos = u_ProbePosition1;
	}
	else if (probe == 2)
	{
		extent = u_ProbeExtent2;
		probe_pos = u_ProbePosition2;
	}

	const float depth = ProbeBoxDepth(world_pos, extent, probe_pos);
	if (extent.w > 0.0f)
		return clamp(depth / extent.w, 0.0f, 1.0f);
	return depth > 0.0f ? 1.0f : 0.0f;
}

/* 逐像素探针选择：
 * - 关闭混合（u_ProbeBlendEnabled = 0）：距离最近硬选（旧行为）
 * - 开启：按盒体权重取前二混合（权重并列时距离近的优先）；权重全零（都出盒）回落距离最近 */
SProbeSelection SelectIBLProbes(vec3 world_pos)
{
	SProbeSelection sel;
	sel.Index0 = 0;
	sel.Index1 = 0;
	sel.Weight0 = 1.0f;
	sel.Weight1 = 0.0f;
	sel.Weighted = false;

	const float dist0 = distance(world_pos, u_ProbePosition0);
	const float dist1 = (u_ProbeCount > 1) ? distance(world_pos, u_ProbePosition1) : 1e30f;
	const float dist2 = (u_ProbeCount > 2) ? distance(world_pos, u_ProbePosition2) : 1e30f;

	if (u_ProbeBlendEnabled == 0)
	{
		if (u_ProbeCount > 1 && dist1 < dist0)
			sel.Index0 = 1;
		if (u_ProbeCount > 2 && dist2 < ((sel.Index0 == 0) ? dist0 : dist1))
			sel.Index0 = 2;
		return sel;
	}

	/* 三候选按（权重降序，并列按距离升序）选择排序，取前二 */
	int index[3] = int[](0, 1, 2);
	float weight[3] = float[](ProbeBoxWeight(world_pos, 0),
		(u_ProbeCount > 1) ? ProbeBoxWeight(world_pos, 1) : 0.0f,
		(u_ProbeCount > 2) ? ProbeBoxWeight(world_pos, 2) : 0.0f);
	float dist[3] = float[](dist0, dist1, dist2);

	for (int i = 0; i < 2; ++i)
	{
		for (int j = i + 1; j < 3; ++j)
		{
			const bool better = weight[j] > weight[i]
				|| (weight[j] == weight[i] && dist[j] < dist[i]);
			if (better)
			{
				const int ti = index[i]; index[i] = index[j]; index[j] = ti;
				const float tw = weight[i]; weight[i] = weight[j]; weight[j] = tw;
				const float td = dist[i]; dist[i] = dist[j]; dist[j] = td;
			}
		}
	}

	sel.Index0 = index[0];
	sel.Index1 = index[1];

	const float sum = weight[0] + weight[1];
	sel.Weighted = weight[0] > 0.0f;
	if (sum > 0.0f)
	{
		sel.Weight0 = weight[0] / sum;
		sel.Weight1 = weight[1] / sum;
	}
	return sel;
}

/* 单探针采样：辐照度与预滤波镜面（按粗糙度选 mip）——与合成拆开，混合路径下
 * 两个探针各调一次（F / kd / BRDF LUT 与探针无关，合成只算一次） */
void SampleProbeIBL(int probe, vec3 N, vec3 R, float roughness, out vec3 irradiance, out vec3 prefiltered)
{
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
}

/* 探针 IBL 合成（支持 top2 混合）：取图在这里，合成走公共库（与前向单探针版同一份） */
vec3 EvaluateProbeIBL(SProbeSelection sel, vec3 N, vec3 V, float roughness, float metallic, vec3 albedo)
{
	roughness = clamp(roughness, 0.0f, 1.0f);
	const float ndv = max(dot(N, V), 0.0f);
	vec3 R = reflect(-V, N);

	vec3 irradiance;
	vec3 prefiltered;
	SampleProbeIBL(sel.Index0, N, R, roughness, irradiance, prefiltered);

	if (sel.Weight1 > 0.0f)
	{
		vec3 irradiance1;
		vec3 prefiltered1;
		SampleProbeIBL(sel.Index1, N, R, roughness, irradiance1, prefiltered1);
		irradiance = mix(irradiance, irradiance1, sel.Weight1);
		prefiltered = mix(prefiltered, prefiltered1, sel.Weight1);
	}

	vec2 brdf = texture(u_BRDFLut, vec2(ndv, roughness)).rg;

	return ComposeIBL(N, V, metallic, albedo, irradiance, prefiltered, brdf);
}

/* 调试分流（Diffuse / Specular / Shadow / Indirect / Probe）：须放在上面这些函数之后 */
#include "../DebugShaders/DebugLighting.glsl"

void main()
{
	SGBufferData gbuffer;
	CalculateGBuffer(gbuffer, vert2frag.TexCoord);

	vec3 world_pos = gbuffer.WorldPosition;
	vec3 n = normalize(gbuffer.WorldNormal);
	vec3 v = normalize(u_ViewPos - world_pos);

	/* 直接光：按本笔光源的类型求光向、衰减与阴影（前向 / 延迟共用） */
	vec3 l;
	float attenuation;
	float shadow;
	EvaluateDirectLight(u_ShadowMap, world_pos, n, l, attenuation, shadow);

	/* 关闭「接受阴影」的模型阴影因子归零（延迟管线只能随 G-Buffer 拿到该标记） */
	shadow *= gbuffer.ReceiveShadow;

	/* 调试分流：命中调试档位直接输出并返回，正常着色时继续往下合成 */
	vec3 lighting_result;
	if (DebugLightingCompose(gbuffer, world_pos, n, v, l, attenuation, shadow, lighting_result))
	{
		OutFragColor = vec4(lighting_result, 1.0f);
		return;
	}

	lighting_result = ComposeDirectLight(l, v, n, gbuffer.Metallic, gbuffer.Roughness, gbuffer.Albedo,
		attenuation, shadow);

	/* 环境项 + 自发光只由负责合成的那一笔光照累加（见 Builtin/DirectLight.glsl） */
	if (u_ComposeAmbientEmission > 0)
	{
		/* 有探针时按逐像素选择取 IBL（权重混合 / 距离最近）；无探针退回平面环境项 */
		if (u_ProbeCount > 0)
		{
			const SProbeSelection selection = SelectIBLProbes(world_pos);
			lighting_result += max(vec3(0.0f),
				EvaluateProbeIBL(selection, n, v, gbuffer.Roughness, gbuffer.Metallic, gbuffer.Albedo));
		}
		else
		{
			/* 平面环境项与前向的无 IBL 回退走同一份合成 */
			lighting_result += max(vec3(0.0f), ComposeFallbackAmbient(gbuffer.Ambient, gbuffer.Albedo, gbuffer.AO));
		}
		lighting_result += gbuffer.Emission;
	}

	OutFragColor = vec4(lighting_result, 1.0f);
}
