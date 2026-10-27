#type vertex
#version 410 core

/* 变换 gizmo 手柄的顶点格式与场景 gizmo 完全一致（共用 MakeGizmoVertexArray 的布局）：
 * a_Color 恒为白，配色走材质的统一色；a_Dir / a_Side 是轴带的屏幕展开参数，实体几何不用。 */
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Color;
layout(location = 2) in vec3 a_Dir;
layout(location = 3) in float a_Side;

#include "../Builtin/Uniforms.glsl"

out vec3 v_Color;

void main()
{
	gl_Position = u_ViewProjectionMat * u_Local2WorldMat * vec4(a_Position, 1.0f);
	v_Color = a_Color;
}


#type fragment
#version 410 core

in vec3 v_Color;

layout (location = 0) out vec4 OutFragColor;

/* 手柄统一色（材质参数写入）：纯色、不参与光照 —— gizmo 是操作提示，不是场景物体。
 * 顶点色的 r 通道叠加透明度：旋转环用它做背侧淡出（CPU 按相机方位逐点算好），
 * 其他手柄的顶点色填 1，乘上去不改变结果。 */
uniform vec4 u_GizmoColor;

void main()
{
	/* 颜色始终用材质色：顶点色的 r 只调制透明度，这样背侧是「透明的轴色」，
	 * 而不是「被乘暗的轴色」（后者看起来发黑） */
	OutFragColor = vec4(u_GizmoColor.rgb, u_GizmoColor.a * v_Color.r);
}
