#type vertex
#version 410 core

/* 顶点声明与内建网格布局对齐（0..4 = 位置 / 法线 / 颜色 / UV / 切线）：
 * 本 shader 只读位置，声明保持一致以免属性槽类型错位 */
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec4 a_Color;
layout(location = 3) in vec3 a_TexCoord;
layout(location = 4) in vec3 a_Tangent;

#include "../Builtin/Uniforms.glsl"

out vec3 v_Direction;

void main()
{
	/* 与 SkyBox.glsl 同一套折叠：只吃视图旋转（天空不跟相机平移），
	 * 网格位置当方向用，深度恒为远平面（Reversed-Z 下是 0），天空始终在最远处 */
	mat4 view_rotation = mat4(mat3(u_ViewMat));
	vec4 position = u_ProjectionMat * view_rotation * vec4(a_Position, 1.0f);
	gl_Position = vec4(position.xy, 0.0f, position.w);

	v_Direction = a_Position;
}


#type fragment
#version 410 core

#include "../Builtin/Math.glsl"
#include "../Builtin/Uniforms.glsl"
#include "../Builtin/SkyUniforms.glsl"

layout(location = 0) out vec4 OutFragColor;

in vec3 v_Direction;

/* 逐像素积分段数：视线 16 段 × 视线采样点的太阳方向 8 段。
 * 段数决定地平线附近的色带精度，不动整体亮度量级；被几何挡住的像素在深度测试就被丢掉了，
 * 真正跑这段积分的只有背景像素。 */
const int kViewSamples = 16;
const int kSunSamples = 8;

/* 太阳 / 月亮盘亮度增益：天上最亮的一小块，压到色调映射的白点上 */
const float kSunDiskGain = 6.0f;
const float kMoonDiskGain = 3.0f;

/* 夜间气辉：多次散射与星光加在一起的底噪，没有它夜空是纯黑 */
const vec3 kNightAirglow = vec3(0.020f, 0.030f, 0.060f);

/* 射线与球的两个根（方向已归一化，二次项系数为 1）。
 * 用数值稳定的求根写法：直接算 -b + sqrt(b²-c) 是两个大数相减，天顶方向的视线对地面球
 * 正好相切，残差正负会让"打到地面没有"逐像素乱翻，整片天空跟着变雪花 */
bool RaySphereRoots(vec3 origin, vec3 dir, float radius, out float t_near, out float t_far)
{
	float b = dot(origin, dir);
	float c = dot(origin, origin) - radius * radius;
	float discriminant = b * b - c;
	if (discriminant < 0.0f)
		return false;

	float sqrt_discriminant = sqrt(discriminant);
	float q = (b > 0.0f) ? -(b + sqrt_discriminant) : (sqrt_discriminant - b);
	t_near = q;
	t_far = (abs(q) > 1.0e-3f) ? (c / q) : q;
	if (t_near > t_far)
	{
		float swap = t_near;
		t_near = t_far;
		t_far = swap;
	}
	return true;
}

/* 射线在球内时离开球面的距离（大气顶） */
float RaySphereFar(vec3 origin, vec3 dir, float radius)
{
	float near_root = 0.0f;
	float far_root = 0.0f;
	if (!RaySphereRoots(origin, dir, radius, near_root, far_root))
		return -1.0f;
	return max(near_root, far_root);
}

/* 射线进入球面的距离（地面）。1 米以内算相切 —— 相机按海平面处理，
 * 不排掉的话整片天空会被自己脚下的地面切掉 */
float RaySphereNear(vec3 origin, vec3 dir, float radius)
{
	float near_root = 0.0f;
	float far_root = 0.0f;
	if (!RaySphereRoots(origin, dir, radius, near_root, far_root))
		return -1.0f;
	return near_root > 1.0f ? near_root : -1.0f;
}

vec3 RotateAroundAxis(vec3 v, vec3 axis, float angle)
{
	float c = cos(angle);
	float s = sin(angle);
	return v * c + cross(axis, v) * s + axis * dot(axis, v) * (1.0f - c);
}

/* 某点沿太阳方向到大气顶的光学厚度，瑞利与米氏分开放（色散只体现在瑞利上）。
 * 太阳被行星挡住时给一个极大值，散射项自然归零 —— 这就是昼夜的分界 */
vec2 SunOpticalDepth(vec3 position, vec3 sun_dir)
{
	if (RaySphereNear(position, sun_dir, u_SkyPlanet.x) > 0.0f)
		return vec2(1.0e9f);

	float distance = RaySphereFar(position, sun_dir, u_SkyPlanet.y);
	if (distance <= 0.0f)
		return vec2(0.0f);

	float step = distance / float(kSunSamples);
	float depth_rayleigh = 0.0f;
	float depth_mie = 0.0f;
	for (int i = 0; i < kSunSamples; ++i)
	{
		vec3 sample_position = position + sun_dir * (step * (float(i) + 0.5f));
		float height = max(length(sample_position) - u_SkyPlanet.x, 0.0f);
		depth_rayleigh += exp(-height / u_SkyRayleigh.w) * step;
		depth_mie += exp(-height / u_SkyMie.y) * step;
	}
	return vec2(depth_rayleigh, depth_mie);
}

/* 单次散射：沿视线积分瑞利 + 米氏，返回散射辐亮度，同时给出视线方向的大气透过率
 * （太阳盘、月亮、星星都乘这一份，才有日出日落时压低变红的效果） */
vec3 Scatter(vec3 view_dir, out vec3 view_transmittance)
{
	/* 相机按海平面处理：几百米的机位高度对整层大气的透过率影响很小 */
	vec3 origin = vec3(0.0f, u_SkyPlanet.x, 0.0f);
	vec3 beta_rayleigh = u_SkyRayleigh.xyz;
	float beta_mie = u_SkyMie.x * u_SkyMie.w;
	float g = u_SkyMie.z;

	float distance = RaySphereFar(origin, view_dir, u_SkyPlanet.y);
	float ground = RaySphereNear(origin, view_dir, u_SkyPlanet.x);
	if (ground > 0.0f)
		distance = min(distance, ground);

	float step = distance / float(kViewSamples);
	float depth_rayleigh = 0.0f;
	float depth_mie = 0.0f;
	vec3 sum_rayleigh = vec3(0.0f);
	vec3 sum_mie = vec3(0.0f);

	for (int i = 0; i < kViewSamples; ++i)
	{
		vec3 position = origin + view_dir * (step * (float(i) + 0.5f));
		float height = max(length(position) - u_SkyPlanet.x, 0.0f);
		float density_rayleigh = exp(-height / u_SkyRayleigh.w);
		float density_mie = exp(-height / u_SkyMie.y);

		depth_rayleigh += density_rayleigh * step;
		depth_mie += density_mie * step;

		/* 到太阳的光学厚度要加上已经走过的这段，日光一路衰减进来 */
		vec2 sun_depth = SunOpticalDepth(position, u_SkySunDirection.xyz);
		vec3 tau = beta_rayleigh * (depth_rayleigh + sun_depth.x)
			+ vec3(beta_mie) * (depth_mie + sun_depth.y);
		vec3 attenuation = exp(-tau);

		sum_rayleigh += density_rayleigh * attenuation * step;
		sum_mie += density_mie * attenuation * step;
	}

	float mu = dot(view_dir, u_SkySunDirection.xyz);
	float phase_rayleigh = 3.0f / (16.0f * PI) * (1.0f + mu * mu);
	float phase_mie = 3.0f / (8.0f * PI) * ((1.0f - g * g) * (1.0f + mu * mu))
		/ ((2.0f + g * g) * pow(1.0f + g * g - 2.0f * g * mu, 1.5f));

	view_transmittance = exp(-(beta_rayleigh * depth_rayleigh + vec3(beta_mie) * depth_mie));
	return u_SkySunRadiance.rgb * (beta_rayleigh * phase_rayleigh * sum_rayleigh
		+ vec3(beta_mie * phase_mie) * sum_mie);
}

float Hash21(vec2 p)
{
	p = fract(p * vec2(123.34f, 456.21f));
	p += dot(p, p + 45.32f);
	return fract(p.x * p.y);
}

/* 一层星点：格点里每格至多一颗，位置 / 尺寸 / 亮度都由格点哈希决定。
 * min_size 是"一像素折算成多少格"：星点比像素还小时按像素兜底并等比压暗，
 * 否则低分辨率下整片星空会被采样漏掉（一闪一闪地消失） */
float StarLayer(vec2 uv, float seed, float density, float min_size)
{
	vec2 cell = floor(uv);
	vec2 offset = uv - cell;
	if (Hash21(cell + seed) > density)
		return 0.0f;

	vec2 star = vec2(Hash21(cell + seed + 7.1f), Hash21(cell + seed + 13.7f));
	float size = mix(0.05f, 0.14f, Hash21(cell + seed + 21.3f));
	float brightness = pow(Hash21(cell + seed + 31.9f), 2.0f);

	float actual_size = max(size, min_size);
	float energy = (size * size) / (actual_size * actual_size);
	return smoothstep(actual_size, 0.0f, length(offset - star)) * brightness * energy;
}

/* 星空：把视线反向转回星空静止的坐标里取样，于是整片星空绕极轴转 */
vec3 Stars(vec3 direction, float night)
{
	vec3 d = RotateAroundAxis(direction, u_SkyStars.xyz, u_SkyStars.w);

	float azimuth = atan(d.z, d.x);
	float elevation = asin(clamp(d.y, -1.0f, 1.0f));
	vec2 uv = vec2(azimuth * INV_2PI, elevation * INV_PI);

	vec2 uv_dense = uv * vec2(420.0f, 210.0f);
	vec2 uv_sparse = uv * vec2(190.0f, 95.0f);
	float stars = StarLayer(uv_dense, 3.0f, 0.035f, fwidth(uv_dense.x) + fwidth(uv_dense.y))
		+ StarLayer(uv_sparse, 17.0f, 0.05f, fwidth(uv_sparse.x) + fwidth(uv_sparse.y)) * 1.8f;

	/* 贴地平线的星点被大气吃掉，别让它们糊在雾里 */
	float horizon_fade = smoothstep(-0.05f, 0.25f, d.y);
	return vec3(stars) * u_SkyPlanet.z * night * horizon_fade;
}

/* 月亮：盘内逐像素重建球面法线，再按太阳方向着色 —— 月相、明暗界线朝向都是算出来的，
 * 不用摆一张贴图也不用按月相切换材质 */
vec3 Moon(vec3 view_dir)
{
	vec3 moon_dir = u_SkyMoonDirection.xyz;
	float angular_radius = u_SkyMoonDirection.w;
	float cos_angle = dot(view_dir, moon_dir);
	if (cos_angle < cos(angular_radius * 1.3f))
		return vec3(0.0f);

	vec3 tangent_offset = view_dir - moon_dir * cos_angle;
	vec3 reference = abs(moon_dir.y) < 0.98f ? vec3(0.0f, 1.0f, 0.0f) : vec3(1.0f, 0.0f, 0.0f);
	vec3 tangent = normalize(cross(reference, moon_dir));
	vec3 bitangent = cross(moon_dir, tangent);

	vec2 local = vec2(dot(tangent_offset, tangent), dot(tangent_offset, bitangent)) / angular_radius;
	float radius = length(local);
	if (radius > 1.0f)
		return vec3(0.0f);

	/* 朝地球这一面的法线：沿 -moon_dir 抬起来再叠切平面偏移 */
	vec3 normal = normalize(-moon_dir * sqrt(max(1.0f - radius * radius, 0.0f))
		+ tangent * local.x + bitangent * local.y);
	float lit = smoothstep(-0.05f, 0.12f, dot(normal, u_SkyMoonLightDirection.xyz));
	float edge = 1.0f - smoothstep(0.85f, 1.0f, radius);
	return u_SkyMoonRadiance.rgb * kMoonDiskGain * lit * edge;
}

/* 曝光 + 电影级色调映射：天空是唯一一档直接进 8bit 目标的自发光，
 * 不走曲线的话正午的天顶会把细节压成一整片 */
vec3 ToneMap(vec3 color)
{
	const float a = 2.51f;
	const float b = 0.03f;
	const float c = 2.43f;
	const float d = 0.59f;
	const float e = 0.14f;
	return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0f, 1.0f);
}

void main()
{
	vec3 view_dir = normalize(v_Direction);
	vec3 origin = vec3(0.0f, u_SkyPlanet.x, 0.0f);
	bool hits_ground = RaySphereNear(origin, view_dir, u_SkyPlanet.x) > 0.0f;

	vec3 transmittance;
	vec3 radiance = Scatter(view_dir, transmittance);

	float sun_elevation = u_SkySunDirection.xyz.y;
	float night = 1.0f - smoothstep(-0.08f, 0.12f, sun_elevation);

	if (!hits_ground)
	{
		/* 盘面按角度比较：cos 空间里做 smoothstep 会把边缘放大成几十度的光斑 */
		float sun_angle = acos(clamp(dot(view_dir, u_SkySunDirection.xyz), -1.0f, 1.0f));
		float disk = 1.0f - smoothstep(u_SkySunDirection.w * 0.85f, u_SkySunDirection.w, sun_angle);
		radiance += u_SkySunRadiance.rgb * transmittance * disk * kSunDiskGain;

		radiance += Moon(view_dir) * transmittance;
		radiance += Stars(view_dir, night);
		radiance += kNightAirglow * night;
	}
	else
	{
		/* 视线打到地面：压暗，空场景的下半球不至于也挂满星星 */
		radiance *= 0.25f;
	}

	OutFragColor = vec4(ToneMap(radiance * u_SkyPlanet.w), 1.0f);
}
