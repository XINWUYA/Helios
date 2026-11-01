#type vertex
#version 410 core

/* Shadow 档的逐光源累加（全屏三角形）：顶点约定与 DeferredShaders/Lighting.glsl 相同 */
struct SVextex2Frag
{
	vec2 TexCoord;
};

layout(location = 0) in vec4 a_Position;
out SVextex2Frag vert2frag;

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
#include "DebugViewCommon.glsl"

struct SVextex2Frag
{
	vec2 TexCoord;
};

in SVextex2Frag vert2frag;

/* 累加编码见 DebugViewCommon.glsl 的 DebugShadowSample，合成端相除得到平均可见度 */
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

	OutFragColor = DebugShadowSample(u_ColorIntensity, attenuation, shadow);
}
