#type vertex
#version 410 core

/* Overdraw 计数（几何重绘）：只取位置 —— 顶点属性契约 location 0..4
 * （位置 / 法线 / 颜色 / UV / 切线），本 shader 只需位置。 */
layout(location = 0) in vec3 a_Position;

#include "../builtin/Uniforms.glsl"

void main()
{
	gl_Position = u_ViewProjectionMat * u_Local2WorldMat * vec4(a_Position, 1.0f);
}


#type fragment
#version 410 core

layout(location = 0) out vec4 OutFragColor;

/* 每个片元向计数纹理累加一份步进（加法混合、不测深度；10 层饱和到 1.0）——
 * 显示层由 DebugView.glsl 的 Overdraw 热力图承担。 */
const float kOverdrawStep = 0.1f;

void main()
{
	OutFragColor = vec4(kOverdrawStep, kOverdrawStep, kOverdrawStep, kOverdrawStep);
}
