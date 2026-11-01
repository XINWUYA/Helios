#type vertex
#version 410 core

/* 草的运动矢量：草没有逐实例矩阵，叶形是在顶点阶段展开的，
 * 所以当前帧与上一帧各展开一次——相机运动由两帧的视图投影体现，
 * 风摆动由两帧的风时间体现，两者合起来才是这东西真正在屏幕上的位移。 */
layout(location = 0) in vec3 a_Position;

layout(location = 5) in vec4 a_InstanceMatrix0;
layout(location = 6) in vec4 a_InstanceMatrix1;
layout(location = 7) in vec4 a_InstanceMatrix2;
layout(location = 8) in vec4 a_InstanceMatrix3;
layout(location = 9) in vec4 a_InstanceParams;

#include "../Builtin/Uniforms.glsl"
#include "../Builtin/GrassBlade.glsl"

/* 1 = 块底面（叶根下的草色垫层），0 = 叶片；与可见着色同名同值 */
uniform int u_FlatPad = 0;              // @property
/* 上一帧的风时间：重算叶片上一帧在哪 */
uniform float u_PrevWindTime = 0.0f;

out vec4 v_CurClip;
out vec4 v_PrevClip;

void main()
{
	const vec3 base = a_InstanceMatrix3.xyz;
	const vec3 right = a_InstanceMatrix0.xyz;
	const vec3 up = a_InstanceMatrix1.xyz;
	const vec3 face = a_InstanceMatrix2.xyz;

	vec3 cur_pos;
	vec3 prev_pos;
	if (u_FlatPad != 0)
	{
		/* 块底面不吃风也不吃脚印，两帧位置一样 */
		cur_pos = base + right * a_Position.x + face * a_Position.z;
		prev_pos = cur_pos;
	}
	else
	{
		vec3 normal_cur;
		vec3 normal_prev;
		cur_pos = ExpandGrassBlade(a_Position, base, right, up, face, a_InstanceParams,
			u_WindTime, normal_cur);
		prev_pos = ExpandGrassBlade(a_Position, base, right, up, face, a_InstanceParams,
			u_PrevWindTime, normal_prev);
	}

	/* 位移用不带抖动的矩阵；光栅化照旧走带抖动的 */
	v_CurClip = u_VelocityViewProjectionMat * vec4(cur_pos, 1.0f);
	v_PrevClip = u_PrevVelocityViewProjectionMat * vec4(prev_pos, 1.0f);
	gl_Position = u_ViewProjectionMat * vec4(cur_pos, 1.0f);
}


#type fragment
#version 410 core

#include "../Builtin/Velocity.glsl"

in vec4 v_CurClip;
in vec4 v_PrevClip;

layout(location = 0) out vec2 OutVelocity;

void main()
{
	OutVelocity = ComputeVelocityFromClip(v_CurClip, v_PrevClip);
}
