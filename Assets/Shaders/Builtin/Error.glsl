#type vertex
#version 410 core

layout(location = 0) in vec3 a_Position;

#include "Uniforms.glsl"

void main()
{
	gl_Position = u_ViewProjectionMat * u_Local2WorldMat * vec4(a_Position, 1.0f);
}


#type fragment
#version 410 core

layout(location = 0) out vec4 OutFragColor;
layout(location = 5) out float ObjectId;

#include "Uniforms.glsl"

void main()
{
	/* 错误材质：材质 / 贴图没配好或加载失败的模型显示品红，方便一眼定位 */
	OutFragColor = vec4(1.0f, 0.0f, 1.0f, 1.0f);

	ObjectId = float(u_ObjectId);
}
