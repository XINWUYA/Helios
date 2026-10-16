#type vertex
#version 410 core

layout(location = 0) in vec4 a_Position;
layout(location = 0) out vec2 v_TexCoord;

void main()
{
	/* 全屏三角形：z=1 即近平面（Reversed-Z），恒通过深度比较 */
	gl_Position = a_Position;

	v_TexCoord = a_Position.xy * 0.5f + 0.5f;
}


#type fragment
#version 410 core

layout(location = 0) out vec4 OutFragColor;
layout(location = 0) in vec2 v_TexCoord;

layout(binding = 0) uniform sampler2D u_SceneColor;
layout(binding = 1) uniform sampler2D u_BloomTexture;
/* 泛光强度：非 opaque uniform 必须显式 location（Windows 走 shaderc） */
layout(location = 0) uniform float u_Intensity = 0.6f;

void main()
{
	vec3 scene = texture(u_SceneColor, v_TexCoord).rgb;
	vec3 bloom = texture(u_BloomTexture, v_TexCoord).rgb;

	OutFragColor = vec4(scene + bloom * u_Intensity, 1.0f);
}
