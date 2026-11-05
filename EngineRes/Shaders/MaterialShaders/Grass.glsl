#type vertex
#version 410 core

/* 草叶模板：x ∈ [-1, 1] 是叶宽方向，y ∈ [0, 1] 从根到梢。
 * 实际形状、弯曲与风都由 Builtin/GrassBlade.glsl 里的展开函数算，
 * 顶点缓冲里只有这一根 4 段的直条。 */
layout(location = 0) in vec3 a_Position;

/* 实例数据：每株一条。前三列已带缩放（x = 叶宽方向、y = 叶高、z = 叶面朝向），
 * 第四列是根部世界坐标；参数 x 保留给 ObjectId、y 接受阴影、z 相位、w 色彩变化 */
layout(location = 5) in vec4 a_InstanceMatrix0;
layout(location = 6) in vec4 a_InstanceMatrix1;
layout(location = 7) in vec4 a_InstanceMatrix2;
layout(location = 8) in vec4 a_InstanceMatrix3;
layout(location = 9) in vec4 a_InstanceParams;

#include "../Builtin/Uniforms.glsl"
#include "../Builtin/GrassBlade.glsl"

/* 1 = 画的是块底面（叶根下的草色垫层），0 = 叶片 */
uniform int u_FlatPad = 0;                         // @group(Ground pad) @property
/* 底面比叶片画得更远：叶片停画之后，草色还能再撑一段 */
uniform float u_PadCullDistance = 160.0f;          // @group(Ground pad) @property

struct SVextex2Frag
{
	vec3 WorldPosition;
	vec3 WorldNormal;
	float Height;
	float ColorSeed;
	float ReceiveShadow;
};

out SVextex2Frag vert2frag;

void main()
{
	const vec3 base = a_InstanceMatrix3.xyz;
	const vec3 right = a_InstanceMatrix0.xyz;
	const vec3 up = a_InstanceMatrix1.xyz;
	const vec3 face = a_InstanceMatrix2.xyz;

	/* 块底面：单位四边形直接铺在叶根高度，不吃风也不吃压弯 */
	if (u_FlatPad != 0)
	{
		/* 太远的块整块退化掉：远景不该留一块漂浮的绿板 */
		if (length(u_ViewPos - base) > u_PadCullDistance)
		{
			gl_Position = vec4(0.0f, 0.0f, -2.0f, 1.0f);
			return;
		}

		const vec3 flat_pos = base + right * a_Position.x + face * a_Position.z;
		gl_Position = u_ViewProjectionMat * vec4(flat_pos, 1.0f);
		vert2frag.WorldNormal = vec3(0.0f, 1.0f, 0.0f);
		vert2frag.WorldPosition = flat_pos;
		vert2frag.Height = 0.0f;
		vert2frag.ColorSeed = a_InstanceParams.w;
		vert2frag.ReceiveShadow = a_InstanceParams.y;
		return;
	}

	/* 远处逐株抽稀：块内密度连续，块边界不会拉出一条竖边 */
	const float view_distance = length(u_ViewPos - base);
	if (GrassBladeFaded(view_distance, a_InstanceParams))
	{
		gl_Position = vec4(0.0f, 0.0f, -2.0f, 1.0f);
		return;
	}

	vec3 blade_normal;
	const vec3 pos = ExpandGrassBlade(a_Position, base, right, up, face, a_InstanceParams,
		GrassWindWeight(view_distance), u_WindPhase, blade_normal);
	gl_Position = u_ViewProjectionMat * vec4(pos, 1.0f);

	vert2frag.WorldNormal = blade_normal;
	vert2frag.WorldPosition = pos;
	vert2frag.Height = a_Position.y;
	vert2frag.ColorSeed = a_InstanceParams.w;
	vert2frag.ReceiveShadow = a_InstanceParams.y;
}


#type fragment
#version 410 core

#include "../Builtin/Math.glsl"
#include "../Builtin/Uniforms.glsl"
#include "../Builtin/BRDF.glsl"
#include "../Builtin/ShadowUtils.glsl"
#include "../Builtin/DirectLight.glsl"
#include "../Builtin/LightCompose.glsl"
#include "../ReflectionProbe/IBLCommon.glsl"
#include "../DebugShaders/DebugForward.glsl"

struct SVextex2Frag
{
	vec3 WorldPosition;
	vec3 WorldNormal;
	float Height;
	float ColorSeed;
	float ReceiveShadow;
};

in SVextex2Frag vert2frag;

layout(location = 0) out vec4 OutFragColor;
/* 草不能按像素拾取，ObjectId 写 -1 与空像素同口径 */
layout(location = 1) out float ObjectId;

uniform vec3 u_BaseColor = vec3(0.09f, 0.20f, 0.05f);          // @group(Surface) @property @type(color)
uniform vec3 u_TipColor = vec3(0.45f, 0.62f, 0.18f);           // @group(Surface) @property @type(color)
uniform vec3 u_TransmissionColor = vec3(0.55f, 0.75f, 0.25f);  // @group(Transmission) @property @type(color)
uniform float u_Transmission = 0.6f;                           // @group(Transmission) @property
uniform float u_TransmissionPower = 4.0f;                      // @group(Transmission) @property
uniform float u_Roughness = 0.75f;                             // @group(Surface) @property
/* 与顶点阶段同一个开关：1 = 块底面（不做透射） */
uniform int u_FlatPad = 0;                                     // @group(Ground pad) @property
/* 环绕光照宽度，与顶点阶段同名同值 */
uniform float u_Wrap = 0.4f;                                   // @group(Surface) @property
/* 叶根的亮度：0 = 根部全黑。低机位看草丛，近处露出来的大多是根部，
 * 这一项决定近处会不会糊成一片黑 */
uniform float u_RootAO = 0.35f;                                // @group(Surface) @property

void main()
{
	const vec3 N = normalize(vert2frag.WorldNormal);
	const vec3 V = normalize(u_ViewPos - vert2frag.WorldPosition);

	/* 根暗梢亮：自遮挡的便宜近似，比加 AO 贴图有效；
	 * 块底面整体压暗一档，叶片稀疏的地方也不会亮过叶片本身 */
	vec3 albedo;
	float ao;
	float thinness;
	if (u_FlatPad != 0)
	{
		albedo = mix(u_BaseColor, u_TipColor, 0.35f) * (0.75f + 0.25f * vert2frag.ColorSeed);
		ao = 1.0f;
		thinness = 0.0f;
	}
	else
	{
		/* 叶高用 sqrt 压一下：中段就到接近全亮。低机位看草丛，近处露出来的多是根部，
		 * 按线性插值会把这一整片压成深色，远处只剩梢部又显得过亮 */
		const float height_t = sqrt(vert2frag.Height);
		albedo = mix(u_BaseColor, u_TipColor, height_t)
			* (0.82f + 0.36f * vert2frag.ColorSeed);
		ao = mix(u_RootAO, 1.0f, height_t);
		thinness = 1.0f;
	}

	vec3 L;
	float attenuation;
	float shadow;
	EvaluateDirectLight(u_ShadowMap, vert2frag.WorldPosition, N, L, attenuation, shadow);
	shadow *= vert2frag.ReceiveShadow;

	/* 草叶很薄，用环绕光照代替硬朗伯：背光面也有一层散射，
	 * 不然 dot(N,L) < 0 的那半直接掉成黑的。镜面项对草意义不大，省掉 */
	const float ndl = clamp((dot(N, L) + u_Wrap) / (1.0f + u_Wrap), 0.0f, 1.0f);
	vec3 color = albedo * ndl * INV_PI * LightFactor(attenuation, shadow);

	/* 透射：逆光时叶肉透出来的那层绿，草地的生命力全在这一项上 */
	const float back_light = pow(max(dot(-L, V), 0.0f), u_TransmissionPower);
	color += u_TransmissionColor * albedo * u_Transmission * back_light * LightFactor(attenuation, shadow)
		* thinness;

	ObjectId = -1.0f;

	/* 调试视图：直接光取上面的环绕漫反射加透射（草不算镜面），环境项多乘一道根部自遮挡 */
	if (u_DebugView != 0)
	{
		SMaterialInput mtl = MakeDebugMaterialInput(albedo, N, u_Roughness, 0.0f);
		mtl.AO = ao;
		SDebugLighting lit = DebugStandardLighting(mtl, N, V, L, attenuation, shadow);
		lit.DirectDiffuse = color;
		lit.DirectSpecular = vec3(0.0f);
		if (u_UseIBL != 0)
		{
			lit.IndirectDiffuse *= ao;
			lit.IndirectSpecular *= ao;
		}
		OutFragColor = DebugForwardOutput(mtl, lit, attenuation, shadow);
		return;
	}

	vec3 ambient;
	if (u_UseIBL != 0)
		ambient = max(vec3(0.0f), EvaluateIBL(N, V, u_Roughness, 0.0f, albedo));
	else
		ambient = ComposeFallbackAmbient(vec3(1.0f), albedo, 1.0f);
	color += ambient * AmbientShadowVisibility(N, L, shadow) * ao;

	OutFragColor = vec4(color, 1.0f);
}
