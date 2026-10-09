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
	/* 是否接受阴影（0/1，来自 ModelComponent）：乘进阴影因子；
	 * 延迟管线里由 GBuffer（GBufferTexture5.a）带给光照阶段 */
	float u_ReceiveShadow;
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
	/* 各级联的 z 空间常数偏移（= ConstantBias / 该级联的光照空间深度跨度）。
	 * ConstantBias 以世界单位计，不同相机 far 下单靠固定 z 偏移无法通用：
	 * 大跨度时偏移过小（阴影被自身遮挡吞掉），小跨度时又过大（阴影整体消失）。 */
	vec4 u_CascadeShadowBias;

	/* ---- 点光/聚光阴影（光照阶段逐光源填充） ---- */
	/* 各阴影面的视图投影矩阵（点光 6 面 / 聚光只用 [0]），面顺序：+X,-X,+Y,-Y,+Z,-Z */
	mat4 u_PunctualShadowMat[MAX_PUNCTUAL_SHADOW_FACE];
	/* x: 阴影面在纹理数组中的起始层；y: 是否投影（0/1）；z: 阴影远平面（= 光源范围）；w: 保留 */
	vec4 u_PunctualShadowParams;
	/* x: 光照范围；y: cos(内锥半角)；z: cos(外锥半角)；w: 阴影投影近平面（世界单位，
	 * 采样端把世界单位的阴影偏移折算到 z 空间用） */
	vec4 u_PunctualLightParams;
};

#endif // UNIFORMS_GLSL