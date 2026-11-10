#type vertex
#version 410 core

/* 草的 G-Buffer 顶点：叶片展开与可见着色同一套函数（GrassBlade.glsl），
 * 额外用上一帧风相位再展开一次，供运动矢量使用。 */

layout(location = 0) in vec3 a_Position;

layout(location = 5) in vec4 a_InstanceMatrix0;
layout(location = 6) in vec4 a_InstanceMatrix1;
layout(location = 7) in vec4 a_InstanceMatrix2;
layout(location = 8) in vec4 a_InstanceMatrix3;
layout(location = 9) in vec4 a_InstanceParams;

#include "../Builtin/Uniforms.glsl"
#include "GrassBlade.glsl"

/* 1 = 块底面（叶根下的草色垫层），0 = 叶片 */
uniform int u_FlatPad = 0;                // @group(Ground pad) @property
uniform float u_PadCullDistance = 160.0f; // @group(Ground pad) @property
/* 上一帧的风相位：算上一帧叶子在哪 */
uniform float u_PrevWindPhase = 0.0f;

struct SVextex2Frag
{
	vec3 WorldPosition;
	vec3 WorldNormal;
	float Height;
	float ColorSeed;
	float ReceiveShadow;
	vec4 CurClip;
	vec4 PrevClip;
};

out SVextex2Frag vert2frag;

void main()
{
	const vec3 base = a_InstanceMatrix3.xyz;
	const vec3 right = a_InstanceMatrix0.xyz;
	const vec3 up = a_InstanceMatrix1.xyz;
	const vec3 face = a_InstanceMatrix2.xyz;

	vert2frag.ColorSeed = a_InstanceParams.w;
	vert2frag.ReceiveShadow = a_InstanceParams.y;

	/* 块底面：单位四边形铺在叶根高度，不吃风也不吃压弯 */
	if (u_FlatPad != 0)
	{
		if (length(u_ViewPos - base) > u_PadCullDistance)
		{
			gl_Position = vec4(0.0f, 0.0f, -2.0f, 1.0f);
			return;
		}

		const vec3 flat_pos = base + right * a_Position.x + face * a_Position.z;
		const vec4 clip = u_VelocityViewProjectionMat * vec4(flat_pos, 1.0f);
		vert2frag.CurClip = clip;
		vert2frag.PrevClip = clip;
		gl_Position = u_ViewProjectionMat * vec4(flat_pos, 1.0f);
		vert2frag.WorldNormal = vec3(0.0f, 1.0f, 0.0f);
		vert2frag.WorldPosition = flat_pos;
		vert2frag.Height = 0.0f;
		return;
	}

	/* 远处逐株抽稀：与可见着色同一判据，丢掉的株两边都不画 */
	const float view_distance = length(u_ViewPos - base);
	if (GrassBladeFaded(view_distance, a_InstanceParams))
	{
		gl_Position = vec4(0.0f, 0.0f, -2.0f, 1.0f);
		return;
	}

	const float wind_weight = GrassWindWeight(view_distance);
	vec3 normal_cur;
	vec3 normal_prev;
	const vec3 pos = ExpandGrassBlade(a_Position, base, right, up, face, a_InstanceParams,
		wind_weight, u_WindPhase, normal_cur);
	const vec3 prev_pos = ExpandGrassBlade(a_Position, base, right, up, face, a_InstanceParams,
		wind_weight, u_PrevWindPhase, normal_prev);

	/* 位移用不带抖动的矩阵，光栅化照旧走带抖动的 */
	vert2frag.CurClip = u_VelocityViewProjectionMat * vec4(pos, 1.0f);
	vert2frag.PrevClip = u_PrevVelocityViewProjectionMat * vec4(prev_pos, 1.0f);
	gl_Position = u_ViewProjectionMat * vec4(pos, 1.0f);
	vert2frag.WorldPosition = pos;
	vert2frag.WorldNormal = normal_cur;
	vert2frag.Height = a_Position.y;
}


#type fragment
#version 410 core

/* 草的 G-Buffer 片元：只算材质，光照留给延迟阶段按模型 ID 分流。
 * 材质参数与 Grass.glsl 同名同义，逐片草地由 per-draw 覆盖带过来。 */

#include "../Builtin/Math.glsl"
#include "../Builtin/Uniforms.glsl"
#include "../Builtin/Velocity.glsl"
#include "../DeferredShaders/ShadingModel.glsl"

// @variant(HELIOS_EDITOR, view)

layout(location = 0) out vec4 GBufferTexture0; /* rgb: Albedo; a: AO */
layout(location = 1) out vec4 GBufferTexture1; /* rgb: WorldNormal(10:10:10); a: 接受阴影(2 bit) */
layout(location = 2) out vec4 GBufferTexture2; /* r: Roughness; g: Metallic; b: Specular; a: 着色模型 ID */
layout(location = 3) out vec4 GBufferTexture3; /* CustomData：透射色 + 强度 */
layout(location = 4) out vec4 GBufferTexture4; /* rgb: Ambient; a: Anisotropy */
layout(location = 5) out vec2 Velocity;
#if HELIOS_EDITOR
layout(location = 6) out float ObjectId;
#endif

struct SVextex2Frag
{
	vec3 WorldPosition;
	vec3 WorldNormal;
	float Height;
	float ColorSeed;
	float ReceiveShadow;
	vec4 CurClip;
	vec4 PrevClip;
};

in SVextex2Frag vert2frag;

uniform vec3 u_BaseColor = vec3(0.09f, 0.20f, 0.05f);          // @group(Surface) @property @type(color)
uniform vec3 u_TipColor = vec3(0.45f, 0.62f, 0.18f);           // @group(Surface) @property @type(color)
uniform vec3 u_TransmissionColor = vec3(0.55f, 0.75f, 0.25f);  // @group(Transmission) @property @type(color)
uniform float u_Transmission = 0.6f;                           // @group(Transmission) @property
uniform float u_Roughness = 0.75f;                             // @group(Surface) @property
/* 与顶点阶段同一个开关：1 = 块底面（不做透射） */
uniform int u_FlatPad = 0;                                     // @group(Ground pad) @property
/* 叶根的亮度：0 = 根部全黑 */
uniform float u_RootAO = 0.35f;                                // @group(Surface) @property

void main()
{
	/* 叶片法线是面法线与向上方向的混合，不随正反面翻转，与 Grass.glsl 同一个取法 */
	const vec3 N = normalize(vert2frag.WorldNormal);

	/* 根暗梢亮：与 Grass.glsl 同一套插值，两边看到的才是同一片草 */
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
		const float height_t = sqrt(vert2frag.Height);
		albedo = mix(u_BaseColor, u_TipColor, height_t)
			* (0.82f + 0.36f * vert2frag.ColorSeed);
		ao = mix(u_RootAO, 1.0f, height_t);
		thinness = 1.0f;
	}

	GBufferTexture0 = vec4(albedo, ao);
	GBufferTexture1 = vec4(N * 0.5f + 0.5f, vert2frag.ReceiveShadow);
	/* 草没有金属度与镜面项，高光通道留 0，alpha 是模型 ID */
	GBufferTexture2 = vec4(u_Roughness, 0.0f, 0.0f, SHADING_MODEL_FOLIAGE);
	/* 块底面的薄度折进强度：底面不参与透射，与 Grass.glsl 的 thinness 同义 */
	GBufferTexture3 = vec4(u_TransmissionColor, u_Transmission * thinness);
	/* 没有 IBL 时延迟用 Ambient 兜底：rgb 是环境色，与 Grass.glsl 的 vec3(1) 一致，a 是各向异性 */
	GBufferTexture4 = vec4(1.0f, 1.0f, 1.0f, 0.0f);

#if HELIOS_EDITOR
	/* 编辑器视图才写拾取通道：点草选中的是草地实体 */
	ObjectId = float(u_ObjectId);
#endif
	Velocity = ComputeVelocityFromClip(vert2frag.CurClip, vert2frag.PrevClip);
}
