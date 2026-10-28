#type vertex
#version 410 core

/* 引擎统一变量块（与 Builtin/Uniforms.glsl 同一份布局）：binding 0 视图、binding 1 物体 */
layout(std140, binding = 0) uniform ViewUniformBuffer
{
	mat4 u_ViewMat;
	mat4 u_ProjectionMat;
	mat4 u_ViewProjectionMat;
	vec3 u_ViewPos;
	uint u_FrameCounter;
};

layout(std140, binding = 1) uniform ObjectUniformBuffer
{
	mat4 u_Local2WorldMat;
	int  u_ObjectId;
	float u_ReceiveShadow;
};

/* 顶点属性按引擎约定：0 位置 / 1 法线 / 2 颜色 / 3 UV / 4 切线 */
layout(location = 0) in vec3 a_Position;

void main()
{
	gl_Position = u_ViewProjectionMat * u_Local2WorldMat * vec4(a_Position, 1.0f);
}


#type fragment
#version 410 core

/* 物体块要在片元里再声明一次（同名同布局）；ObjectId 供视口点击拾取 */
layout(std140, binding = 1) uniform ObjectUniformBuffer
{
	mat4 u_Local2WorldMat;
	int  u_ObjectId;
	float u_ReceiveShadow;
};

/* 带 @property 的 uniform 会出现在材质面板；@type(color) 让 vec4 画成取色器 */
uniform vec4 u_Color = vec4(0.8f, 0.8f, 0.9f, 1.0f); // @property @type(color)
uniform float u_Opacity = 1.0f; // @property @range(0, 1)

layout(location = 0) out vec4 OutFragColor;
layout(location = 1) out float ObjectId;

void main()
{
	OutFragColor = u_Color * vec4(1.0f, 1.0f, 1.0f, u_Opacity);
	ObjectId = float(u_ObjectId);
}
