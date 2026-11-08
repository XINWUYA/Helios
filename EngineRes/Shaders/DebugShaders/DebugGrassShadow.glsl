#type vertex
#version 410 core

/* 草的调试重绘：Shadow 档。可见度要采阴影图，单独一份着色器 —— 其余档位不背这个采样器 */
#include "../Grass/GrassBladeVertex.glsl"


#type fragment
#version 410 core

#include "../Builtin/Uniforms.glsl"
#include "../Builtin/DirectLight.glsl"

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

void main()
{
	const vec3 n = normalize(vert2frag.WorldNormal);
	vec3 l;
	float attenuation;
	float shadow;
	EvaluateDirectLight(u_ShadowMap, vert2frag.WorldPosition, n, l, attenuation, shadow);

	/* 与合成端同口径：1 = 受光，0 = 遮挡；草可以逐片关掉接收阴影 */
	OutFragColor = vec4(vec3(1.0f - shadow * vert2frag.ReceiveShadow), 1.0f);
}
