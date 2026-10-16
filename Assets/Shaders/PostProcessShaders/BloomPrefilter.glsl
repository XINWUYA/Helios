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
/* 亮部提取阈值：非 opaque uniform 必须显式 location（Windows 走 shaderc） */
layout(location = 0) uniform float u_Threshold = 0.8f;

void main()
{
	vec3 color = texture(u_SceneColor, v_TexCoord).rgb;

	/* 软膝阈值（URP 口径，膝宽 0.5×阈值）：亮部平滑提取，阈值边缘不硬切 */
	float knee = u_Threshold * 0.5f;
	float brightness = max(color.r, max(color.g, color.b));
	float soft = clamp(brightness - u_Threshold + knee, 0.0f, 2.0f * knee);
	soft = soft * soft / (4.0f * knee + 1e-4f);
	float contribution = max(soft, brightness - u_Threshold) / max(brightness, 1e-4f);

	OutFragColor = vec4(color * contribution, 1.0f);
}
