#type vertex
#version 410 core

layout(location = 0) in vec3 a_Position;

#include "../Builtin/Uniforms.glsl"

void main()
{
	/* 模型线框叠加：只取位置，直接过视图投影 */
	gl_Position = u_ViewProjectionMat * u_Local2WorldMat * vec4(a_Position, 1.0f);
}


#type fragment
#version 410 core

layout(location = 0) out vec4 OutFragColor;

/* 线框颜色（材质参数写入）：纯色、不参与光照 —— 线框是"看结构"用的调试视图 */
layout(location = 0) uniform vec4 u_WireframeColor;

void main()
{
	OutFragColor = u_WireframeColor;
}
