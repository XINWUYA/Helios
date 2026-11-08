#type vertex
#version 410 core

/* 调试读数整段只在调试变体里编进来，正常着色不带那份代码。
 * 变体值由渲染通道逐笔给（0 = 正常，1 = 出读数），具体档位仍走 u_DebugView */
// @variant(DEBUG_VIEW, external)

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec4 a_Color;
layout(location = 3) in vec3 a_TexCoord;
layout(location = 4) in vec3 a_Tangent;

#include "../Builtin/Uniforms.glsl"

struct SVextex2Frag
{
	vec3 WorldPosition;
	vec3 WorldNormal;
	vec4 BaseColor;
	vec2 TexCoord;
#if MTL_NORMAL || MTL_PARALLAX
	vec3 WorldTangent;
#endif
};

out SVextex2Frag vert2frag;

void main()
{
	vec4 world_pos = u_Local2WorldMat * vec4(a_Position, 1.0f);

	gl_Position = u_ViewProjectionMat * world_pos;

	vert2frag.WorldPosition = world_pos.xyz;
	vert2frag.BaseColor     = a_Color;
	vert2frag.TexCoord      = a_TexCoord.xy;

	/* 法线矩阵：处理非等比缩放。切线只有法线图 / 视差档才用得上，关掉就不算也不传 */
	mat3 normal_mat = transpose(inverse(mat3(u_Local2WorldMat)));
	vert2frag.WorldNormal  = normalize(normal_mat * a_Normal);
#if MTL_NORMAL || MTL_PARALLAX
	vert2frag.WorldTangent = normalize(normal_mat * a_Tangent);
#endif
}


#type fragment
#version 410 core

#include "../Builtin/Math.glsl"
#include "../Builtin/Uniforms.glsl"
#include "../Builtin/BRDF.glsl"
#include "../Builtin/MaterialInput.glsl"
#include "../Builtin/MaterialLit.glsl"
#include "../Builtin/ShadowUtils.glsl"
#include "../Builtin/DirectLight.glsl"
#include "../Builtin/LightCompose.glsl"
#include "../ReflectionProbe/IBLCommon.glsl"
#if DEBUG_VIEW
#include "../DebugShaders/DebugForward.glsl"
#endif

/* 不透明度乘数：透明队列的材质靠它整体调淡；不透明队列不混合，改了看不见 */
uniform float u_Opacity = 1.0f; // @group(Surface) @property

struct SVextex2Frag
{
	vec3 WorldPosition;
	vec3 WorldNormal;
	vec4 BaseColor;
	vec2 TexCoord;
#if MTL_NORMAL || MTL_PARALLAX
	vec3 WorldTangent;
#endif
};

in SVextex2Frag vert2frag;

layout(location = 0) out vec4 OutFragColor;
layout(location = 1) out float ObjectId;

void main()
{
	SMaterialInput mtl;
	/* 变体关掉切线时传零向量，MaterialLit 只在 TBN 分支里读它 */
#if MTL_NORMAL || MTL_PARALLAX
	const vec3 world_tangent = vert2frag.WorldTangent;
#else
	const vec3 world_tangent = vec3(0.0f);
#endif
	CalculateLitMaterial(mtl, vert2frag.WorldPosition, vert2frag.TexCoord,
		vert2frag.BaseColor, vert2frag.WorldNormal, world_tangent);

	vec3 N = mtl.Normal;
	vec3 V = normalize(u_ViewPos - vert2frag.WorldPosition);

	/* 直接光：按本笔光源的类型（方向光 / 点光 / 聚光）求光向、衰减与阴影 */
	vec3 L;
	float attenuation;
	float shadow;
	EvaluateDirectLight(u_ShadowMap, vert2frag.WorldPosition, N, L, attenuation, shadow);
	/* 模型关闭「接受阴影」时阴影因子归零 */
	shadow *= u_ReceiveShadow;

	ObjectId = float(u_ObjectId);

	/* 调试视图：走调试变体时才编进来，命中就直接输出读数 */
#if DEBUG_VIEW
	if (u_DebugView != 0)
	{
		OutFragColor = DebugForwardOutput(mtl, DebugStandardLighting(mtl, N, V, L, attenuation, shadow),
			attenuation, shadow);
		return;
	}
#endif

	/* 间接光：有 IBL 绑定时用反射探针环境贴图，否则退回平面环境项；
	 * 环境项 + 自发光只在合成的那一笔里累加（其余笔只叠直接光） */
	vec3 color = ComposeDirectLight(L, V, N, mtl.Metallic, mtl.Roughness, mtl.Albedo,
		attenuation, shadow);
	if (u_ComposeAmbientEmission > 0)
	{
		vec3 ambient;
		if (u_UseIBL != 0)
			ambient = max(vec3(0.0f), EvaluateIBL(N, V, mtl.Roughness, mtl.Metallic, mtl.Albedo));
		else
			ambient = max(vec3(0.0f), ComposeFallbackAmbient(mtl.Ambient, mtl.Albedo, mtl.AO));

		color += ambient * AmbientShadowVisibility(N, L, shadow) + mtl.Emission;
	}

	OutFragColor = vec4(color, mtl.Opacity * u_Opacity);
}
