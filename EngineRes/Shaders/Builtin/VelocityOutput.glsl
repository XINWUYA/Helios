#ifndef VELOCITY_OUTPUT_GLSL
#define VELOCITY_OUTPUT_GLSL

/* 运动矢量的片元阶段：普通网格 / 草 / 地形三条路径的顶点各算各的裁剪坐标，
 * 到这一步完全一样，除一下就是屏幕位移。 */

#include "Velocity.glsl"

in vec4 v_CurClip;
in vec4 v_PrevClip;

layout(location = 0) out vec2 OutVelocity;

void main()
{
	OutVelocity = ComputeVelocityFromClip(v_CurClip, v_PrevClip);
}

#endif // VELOCITY_OUTPUT_GLSL
