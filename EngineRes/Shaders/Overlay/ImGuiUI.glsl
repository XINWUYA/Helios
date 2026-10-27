#type vertex
#version 410 core

layout(location = 0) in vec2 a_Position;
layout(location = 1) in vec2 a_TexCoord;
layout(location = 2) in vec4 a_Color;

out vec2 v_TexCoord;
out vec4 v_Color;

layout(std140, binding = 4) uniform UIUniformBuffer
{
	mat4 u_Projection;
};

void main()
{
	gl_Position = u_Projection * vec4(a_Position, 0.0, 1.0);
	v_TexCoord = a_TexCoord;
	v_Color = a_Color;
}

#type fragment
#version 410 core

in vec2 v_TexCoord;
in vec4 v_Color;

layout(location = 0) out vec4 OutFragColor;

uniform sampler2D u_FontTexture;

void main()
{
	vec4 texColor = texture(u_FontTexture, v_TexCoord);
	OutFragColor = v_Color * texColor;
}
