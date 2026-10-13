#type vertex
#version 410 core

layout(location = 0) in vec4 a_Position;
layout(location = 0) out vec2 v_TexCoord;

void main()
{
	gl_Position = a_Position;

	v_TexCoord = a_Position.xy * 0.5f + 0.5f;
}


#type fragment
#version 410 core

layout(location = 0) out vec4 OutFragColor;
layout(location = 0) in vec2 v_TexCoord;

layout(binding = 0) uniform sampler2D u_Source;

/* 源纹理纹素尺寸：非 opaque uniform 必须显式 location（Windows GLSL→SPIR-V）。
 * 本 stage 无其它非 opaque uniform，独占 0。 */
layout(location = 0) uniform vec2 u_TexelSize = vec2(0.0f);

void main()
{
	/* 4 点盒式降采样：抽头落在源纹素中心（±0.5 源纹素），四等权 = 精确 2×2 盒平均。
	 * 单点闪烁由升采样端的 tent 模糊承担。 */
	vec2 half_texel = u_TexelSize * 0.5f;

	vec3 color = texture(u_Source, v_TexCoord + vec2(-half_texel.x, -half_texel.y)).rgb;
	color += texture(u_Source, v_TexCoord + vec2(half_texel.x, -half_texel.y)).rgb;
	color += texture(u_Source, v_TexCoord + vec2(-half_texel.x, half_texel.y)).rgb;
	color += texture(u_Source, v_TexCoord + vec2(half_texel.x, half_texel.y)).rgb;

	OutFragColor = vec4(color * 0.25f, 1.0f);
}
