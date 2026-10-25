#type vertex
#version 410 core

layout(location = 0) in vec4 a_Position;
out vec2 v_TexCoord;

void main()
{
	/* 全屏三角形：z=1 即近平面（Reversed-Z），恒通过深度比较 */
	gl_Position = a_Position;

	v_TexCoord = a_Position.xy * 0.5f + 0.5f;
}


#type fragment
#version 410 core

layout(location = 0) out vec4 OutFragColor;
in vec2 v_TexCoord;

uniform sampler2D u_Source;

void main()
{
	/* 直通拷贝：后处理链末端的呈现 blit */
	OutFragColor = vec4(texture(u_Source, v_TexCoord).rgb, 1.0f);
}
