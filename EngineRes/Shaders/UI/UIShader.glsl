#type vertex
#version 410 core

layout(location = 0) in vec2 a_Position;
layout(location = 1) in vec2 a_TexCoord;
layout(location = 2) in vec4 a_Color;

out vec2 v_TexCoord;
out vec4 v_Color;

/* 目标像素空间的屏幕正交矩阵：RmlUi 的坐标原点在左上角 */
uniform mat4 u_Projection;

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

uniform sampler2D u_Texture;

void main()
{
	/* 颜色与纹理都按预乘 alpha 存（RmlUi 6 的约定），乘完直接写，混合用 One / OneMinusSrcAlpha */
	OutFragColor = v_Color * texture(u_Texture, v_TexCoord);
}
