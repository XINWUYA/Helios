#ifndef UNIFORMS_GLSL
#define UNIFORMS_GLSL

/* 光照视图投影矩阵数组的最大长度（与 C++ 侧 LightUniformData 保持一致） */
#define MAX_LIGHT_VIEW_PROJ 4

/* 点光立方体阴影的面数上限（与 C++ 侧 LightUniformData 保持一致） */
#define MAX_PUNCTUAL_SHADOW_FACE 6



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
};

layout(std140, binding = 3) uniform LightUniformBuffer
{
	/* 级联阴影的光照视图投影矩阵数组（最多 MAX_LIGHT_VIEW_PROJ 个） */
	mat4 u_LightViewProjectionMat[MAX_LIGHT_VIEW_PROJ];
	vec4 u_ColorIntensity; /* rgb： Color; a: Intensity */
	vec3 u_LightDir;       /* 光传播方向（方向光 / 聚光） */
	uint u_LightType;
	vec3 u_LightPos;       /* 光源世界位置（点光 / 聚光） */
	uint u_CascadeCount;   /* 实际使用的级联数量（0 = 该光源无级联阴影） */
	vec4 u_CascadeSplits;
	float u_ShadowBias;    /* 阴影深度偏移（来自 ShadowMapInfo::ConstantBias），缓解阴影失真 */

	/* ---- 点光/聚光阴影（光照阶段逐光源填充） ---- */
	/* 各阴影面的视图投影矩阵（点光 6 面 / 聚光只用 [0]），面顺序：+X,-X,+Y,-Y,+Z,-Z */
	mat4 u_PunctualShadowMat[MAX_PUNCTUAL_SHADOW_FACE];
	/* x: 阴影面在纹理数组中的起始层；y: 是否投影（0/1）；z: 阴影远平面（= 光源范围）；w: 保留 */
	vec4 u_PunctualShadowParams;
	/* x: 光照范围；y: cos(内锥半角)；z: cos(外锥半角)；w: 保留 */
	vec4 u_PunctualLightParams;
};

#endif // UNIFORMS_GLSL