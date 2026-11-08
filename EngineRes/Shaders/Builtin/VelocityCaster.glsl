#type vertex
#version 410 core

/* 运动矢量投射：把不透明几何再画一遍，只算屏幕位移。
 * 走的是和场景绘制同一份对象矩阵，深度测试对着场景已经写好的深度（只测不写），
 * 被挡住的像素不会拿到前景物体的速度。
 * 顶点变换与材质着色器不完全一致，深度可能差 1 个 ulp，所以用 GreaterEqual 而不是 Equal。 */
layout(location = 0) in vec3 a_Position;

#include "Uniforms.glsl"

out vec4 v_CurClip;
out vec4 v_PrevClip;

void main()
{
	const vec4 world = u_Local2WorldMat * vec4(a_Position, 1.0f);
	const vec4 prev_world = u_PrevLocalToWorldMat * vec4(a_Position, 1.0f);

	/* 位移用不带抖动的矩阵；光栅化照旧走带抖动的，落到本帧的像素上 */
	v_CurClip = u_VelocityViewProjectionMat * world;
	v_PrevClip = u_PrevVelocityViewProjectionMat * prev_world;
	gl_Position = u_ViewProjectionMat * world;
}


#type fragment
#version 410 core

#include "VelocityOutput.glsl"
