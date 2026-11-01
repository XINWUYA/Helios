#type vertex
#version 410 core

layout(location = 0) in vec4 a_Position;

void main()
{
	gl_Position = a_Position;
}


#type fragment
#version 410 core

/* 运动矢量附件的初值：全部置成"不动"，几何随后覆盖上去。
 * 运动矢量通道要保留场景深度（Load），没法在通道开始时清颜色，就用这一笔代替。 */
layout(location = 0) out vec2 OutVelocity;

void main()
{
	OutVelocity = vec2(0.0f);
}
