#ifndef VELOCITY_GLSL
#define VELOCITY_GLSL

/* 由两个裁剪空间位置算屏幕空间速度（UV 单位）。
 * 两个位置都得用不带抖动的视图投影算：抖动是同一表面点换了个像素，
 * 算进位移里会让整屏每帧都在动，重采样反倒更抖。 */
vec2 ComputeVelocityFromClip(vec4 cur_clip, vec4 prev_clip)
{
	/* w 趋零说明点在相机背后，这种像素没法重投影，当作不动 */
	if (abs(cur_clip.w) < 1e-6f || abs(prev_clip.w) < 1e-6f)
		return vec2(0.0f);

	return (cur_clip.xy / cur_clip.w - prev_clip.xy / prev_clip.w) * 0.5f;
}

#endif // VELOCITY_GLSL
