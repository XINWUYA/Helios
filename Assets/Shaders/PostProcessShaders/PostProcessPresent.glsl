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

void main()
{
	OutFragColor = vec4(texture(u_Source, v_TexCoord).rgb, 1.0f);
}
