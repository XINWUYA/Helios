#type vertex
#version 410 core

/* 草的阴影投射：顶点展开跟可见着色共用 Builtin/GrassBlade.glsl 的那一份，
 * 风与脚印都算进去，影子才跟得上草的动作。
 * 不需要法线、不需要颜色，只要位置。 */
layout(location = 0) in vec3 a_Position;

layout(location = 5) in vec4 a_InstanceMatrix0;
layout(location = 6) in vec4 a_InstanceMatrix1;
layout(location = 7) in vec4 a_InstanceMatrix2;
layout(location = 8) in vec4 a_InstanceMatrix3;
layout(location = 9) in vec4 a_InstanceParams;

/* 与 ShadowCaster.glsl 同一个约定：>= 0 取级联矩阵，-1 取单面矩阵 */
uniform int u_CascadeIndex;
uniform mat4 u_ShadowPassMat;

/* 距离抽稀：叶根到相机的水平距离在 full 以内全画，往外按实例自带的随机数逐株丢掉，
 * fade 处密度归零。逐株判定而不是整块截断，相机移动时是慢慢变稀，不会成块跳 */
uniform vec3 u_GrassShadowEye = vec3(0.0f);
uniform float u_GrassShadowFullRadius = 20.0f;
uniform float u_GrassShadowFadeRadius = 34.0f;

#include "../Builtin/Uniforms.glsl"
#include "../Builtin/GrassBlade.glsl"

/* 实例的两个随机数（相位 / 色彩种子）凑一个 [0,1) 阈值：同一株每帧恒定，不会闪 */
float GrassShadowDither(vec4 params)
{
	return fract(params.z * 91.37f + params.w * 47.11f);
}

/* 密度与阈值一起送给片元：丢叶子放在片元里做，不依赖各后端对裁剪体外顶点的处理 */
out float v_GrassShadowDensity;
out float v_GrassShadowDither;

void main()
{
	/* 距离抽稀：叶根到相机的水平距离在 full 以内全画，往外按实例自带的随机数逐株丢掉，
	 * fade 处密度归零。逐株判定而不是整块截断，相机移动时是慢慢变稀，不会成块跳 */
	const float distance = length(a_InstanceMatrix3.xz - u_GrassShadowEye.xz);
	v_GrassShadowDensity = (1.0f
		- smoothstep(u_GrassShadowFullRadius, u_GrassShadowFadeRadius, distance))
		* u_BladeDensityScale;
	v_GrassShadowDither = GrassShadowDither(a_InstanceParams);

	vec3 blade_normal;
	const vec3 pos = ExpandGrassBlade(a_Position, a_InstanceMatrix3.xyz, a_InstanceMatrix0.xyz,
		a_InstanceMatrix1.xyz, a_InstanceMatrix2.xyz, a_InstanceParams,
		GrassWindWeight(length(a_InstanceMatrix3.xyz - u_GrassShadowEye)), u_WindTime, blade_normal);

	const mat4 light_view_projection_mat = (u_CascadeIndex >= 0)
		? u_LightViewProjectionMat[u_CascadeIndex]
		: u_ShadowPassMat;
	gl_Position = light_view_projection_mat * vec4(pos, 1.0f);
}


#type fragment
#version 410 core

in float v_GrassShadowDensity;
in float v_GrassShadowDither;

void main()
{
	/* 密度不足的叶子整片丢掉：影子随距离连续变稀，而不是一刀切成两档 */
	if (v_GrassShadowDither > v_GrassShadowDensity)
		discard;
}
