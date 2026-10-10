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

uniform vec2 u_TexelSize = vec2(0.0f);
uniform float u_Scatter = 0.7f;

void main()
{
	/* 3×3 tent（权重 1/2/1 双轴外积）：偏移 = 源（下一级）纹素，覆盖范围随级数翻倍；
	 * 结果 × Scatter 后由加法混合回叠到上一级。 */
	vec2 t = u_TexelSize;

	vec3 color = texture(u_Source, v_TexCoord + vec2(-t.x, -t.y)).rgb;
	color += texture(u_Source, v_TexCoord + vec2(0.0f, -t.y)).rgb * 2.0f;
	color += texture(u_Source, v_TexCoord + vec2(t.x, -t.y)).rgb;
	color += texture(u_Source, v_TexCoord + vec2(-t.x, 0.0f)).rgb * 2.0f;
	color += texture(u_Source, v_TexCoord).rgb * 4.0f;
	color += texture(u_Source, v_TexCoord + vec2(t.x, 0.0f)).rgb * 2.0f;
	color += texture(u_Source, v_TexCoord + vec2(-t.x, t.y)).rgb;
	color += texture(u_Source, v_TexCoord + vec2(0.0f, t.y)).rgb * 2.0f;
	color += texture(u_Source, v_TexCoord + vec2(t.x, t.y)).rgb;

	OutFragColor = vec4(color * (u_Scatter / 16.0f), 1.0f);
}
