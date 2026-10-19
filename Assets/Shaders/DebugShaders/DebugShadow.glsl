#type vertex
#version 410 core

/* Shadow 档的逐光源累加（全屏三角形）：顶点约定与 DeferredShaders/Lighting.glsl 相同 */
struct SVextex2Frag
{
	vec2 TexCoord;
};

layout(location = 0) in vec4 a_Position;
layout(location = 0) out SVextex2Frag vert2frag;

void main()
{
	gl_Position = a_Position;

	vert2frag.TexCoord = a_Position.xy * 0.5f + 0.5f;
}


#type fragment
#version 410 core

#include "../Builtin/Uniforms.glsl"
#include "../DeferredShaders/GBufferCommon.glsl"
#include "../Builtin/ShadowUtils.glsl"
#include "../Builtin/LightCompose.glsl"
#include "../Builtin/DirectLight.glsl"

struct SVextex2Frag
{
	vec2 TexCoord;
};

layout(location = 0) in SVextex2Frag vert2frag;

/* rg：可见度 × 权重、权重。逐光源加法累加，合成端相除得到平均可见度
 * （见 DebugShaders/DebugView.glsl 的 Shadow 档） */
layout(location = 0) out vec4 OutFragColor;

void main()
{
	SGBufferData gbuffer;
	CalculateGBuffer(gbuffer, vert2frag.TexCoord);

	const vec3 n = normalize(gbuffer.WorldNormal);
	vec3 l;
	float attenuation;
	float shadow;
	EvaluateDirectLight(u_ShadowMap, gbuffer.WorldPosition, n, l, attenuation, shadow);
	shadow *= gbuffer.ReceiveShadow;

	/* 权重只看这盏灯照不照得到：颜色亮度 × 强度 × 衰减。不乘 N·L —— 法线贴图的沟槽
	 * 会把 N·L 压到 0，这些像素被踢出平均后合成端回退成白，就成了阴影里的白点 */
	const float luma = dot(u_ColorIntensity.rgb, vec3(0.2126f, 0.7152f, 0.0722f));
	const float weight = luma * u_ColorIntensity.a * attenuation;

	OutFragColor = vec4(weight * (1.0f - shadow), weight, 0.0f, 0.0f);
}
