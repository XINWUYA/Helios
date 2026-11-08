#ifndef GRASS_DENSITY_GLSL
#define GRASS_DENSITY_GLSL

/* 草地的距离抽稀：纯净数学，不带任何 uniform 与属性，可见着色、速度、阴影、
 * 调试视图和 GPU 剔除的计算着色器都 include 这一份，曲线只有一个来源。
 *
 * 抽稀必须逐株判定。按块算一个系数去截断实例数，块与块之间就是台阶：
 * 20 米一块，相邻块的密度差直接画成一条竖边，越远越明显。 */

/* 每株的抽样值：相位与色彩种子混一下，跟风的相位解耦，免得抽稀图案跟着摆动走 */
float GrassDensityDither(vec4 params)
{
	return fract(params.z * 91.37f + params.w * 47.11f);
}

/* 密度曲线：t = 0 满密度，t = 1 归零。
 * 主体按三次方从 1 降到 min_scale（近处保持密），再乘一个到尾端归零的因子，
 * 让整段距离都在慢慢变稀 —— 尾部太短的话，视觉上还是一条边。
 * 归零是必须的：块按剔除距离整块消失时密度已经先到 0，不会整片跳掉 */
float GrassDensityCurve(float t, float min_scale)
{
	const float floor_density = max(min_scale, 0.0f);
	const float body = mix(1.0f, floor_density, t * t * t);
	return body * (1.0f - t * t);
}

/* 这一株的保留概率：不含随机判定，GPU 剔除要往上乘预算系数，所以单独拆一份 */
float GrassDensityWeight(float distance, float full_distance, float fade_distance,
	float min_scale)
{
	if (distance <= full_distance)
		return 1.0f;
	return GrassDensityCurve(clamp((distance - full_distance)
		/ max(fade_distance - full_distance, 1e-3f), 0.0f, 1.0f), min_scale);
}

/* 这一株留不留：distance 是叶根到相机的距离，参数含义与 GrassComponent 一致。
 * density_scale 是画质档的全局系数，乘在阈值上，0 就是整片不画 */
float GrassDensityKeep(float distance, float random_value, float full_distance,
	float fade_distance, float min_scale, float density_scale)
{
	return random_value < GrassDensityWeight(distance, full_distance, fade_distance, min_scale)
		* max(density_scale, 0.0f) ? 1.0f : 0.0f;
}

/* 草影的保留概率：按水平距离，full 以内全留，fade 处归零。
 * 阴影着色与 GPU 剔除共用这一条，影子的疏密只有一个来源 */
float GrassShadowWeight(float distance, float full_distance, float fade_distance)
{
	return 1.0f - smoothstep(full_distance, max(fade_distance, full_distance + 1e-3f), distance);
}

#endif // GRASS_DENSITY_GLSL
