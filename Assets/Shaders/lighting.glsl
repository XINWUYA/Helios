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

struct SVextex2Frag
{
	vec2 TexCoord;
};

layout(location = 0) out vec4 OutFragColor;
layout(location = 0) in SVextex2Frag vert2frag;

void main()
{
	SGBufferData gbuffer;
	CalculateGBuffer(gbuffer, vert2frag.TexCoord);

	vec3 l = -normalize(u_LightDir);
	vec3 v = normalize(u_ViewPos - gbuffer.WorldPosition);

	vec3 lighting_result = max(vec3(0.0f), BRDF(l, v, gbuffer.WorldNormal, gbuffer.Metallic, gbuffer.Roughness, gbuffer.Albedo)) * u_ColorIntensity.rgb * u_ColorIntensity.a;
	/* 编辑器延迟 Pass 未生成或绑定 ShadowMap，因此此处只合成无阴影光照。 */
	lighting_result += max(vec3(0.0f), gbuffer.Ambient * gbuffer.Albedo * gbuffer.AO);
	lighting_result += gbuffer.Emission;

	OutFragColor = vec4(lighting_result, 1.0f);
}
