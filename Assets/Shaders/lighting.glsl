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

#include "builtin/Uniforms.glsl"
#include "builtin/GBuffer.glsl"
#include "builtin/Math.glsl"
#include "builtin/BRDF.glsl"
#include "builtin/ShadowUtils.glsl"

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

/* 第一笔光照才合成环境光 / 自发光：
 * 光照按"每光源一笔全屏加法"叠加，ambient + emission 与光源无关，
 * 多光源场景里只能由其中一笔（约定为第一笔）负责，否则会随光源数重复叠加。 */
layout(location = 0) uniform int u_ComposeAmbientEmission = 0;

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

/* 平滑范围衰减：距离在 [0, range] 内按 (1 - d/range)^2 衰减，到 range 处归零。
 * 不用物理逆平方 —— 引擎的强度没有单位体系，逆平方在 d→0 时数值发散，
 * 会使贴近光源的表面过曝；"范围窗"形状观感稳定，且与阴影远平面同源。 */
float RangeAttenuation(float distance, float range)
{
	float t = clamp(1.0f - distance / max(range, 1e-4f), 0.0f, 1.0f);
	return t * t;
}

void main()
{
	SGBufferData gbuffer;
	CalculateGBuffer(gbuffer, vert2frag.TexCoord);

	vec3 world_pos = gbuffer.WorldPosition;
	vec3 n = normalize(gbuffer.WorldNormal);
	vec3 v = normalize(u_ViewPos - world_pos);

	/* 按光源类型求"指向光源"的向量 l、衰减 attenuation 与阴影因子 shadow */
	vec3 l;
	float attenuation = 1.0f;
	float shadow = 0.0f;

	if (u_LightType == 0u)
	{
		/* 方向光：u_LightDir 为传播方向；仅当该光源负责级联阴影时才采样
		 * （CascadeCount = 0 表示无级联数据，矩阵是单位阵，采样会得到错误遮挡）。 */
		l = -normalize(u_LightDir);
		if (u_CascadeCount > 0u)
		{
			vec3 view_pos = (u_ViewMat * vec4(world_pos, 1.0f)).xyz;
			shadow = CalculateShadow(u_ShadowMap, world_pos, view_pos, n, l);
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
			shadow = CalculatePointShadow(u_ShadowMap, world_pos, u_LightPos);
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
			shadow = CalculateSpotShadow(u_ShadowMap, world_pos);
	}

	/* 模型关闭「接受阴影」时阴影因子归零（延迟管线只能随 G-Buffer 拿到该标记） */
	shadow *= gbuffer.ReceiveShadow;

	vec3 direct = max(vec3(0.0f), BRDF(l, v, n, gbuffer.Metallic, gbuffer.Roughness, gbuffer.Albedo));
	direct *= u_ColorIntensity.rgb * u_ColorIntensity.a * attenuation;
	direct *= (1.0f - shadow * 0.8f);

	vec3 lighting_result = direct;

	/* 环境项 + 自发光只由第一笔光照合成（见 u_ComposeAmbientEmission 注释） */
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
			lighting_result += max(vec3(0.0f), gbuffer.Ambient * gbuffer.Albedo * gbuffer.AO);
		}
		lighting_result += gbuffer.Emission;
	}

	OutFragColor = vec4(lighting_result, 1.0f);
}
