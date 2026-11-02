#ifndef GRASS_BLADE_GLSL
#define GRASS_BLADE_GLSL

#include "Uniforms.glsl"
#include "GrassDensity.glsl"

/* 草叶的形变数学：可见着色与阴影投影共用这一份，两边算出来的形状必须一模一样，
 * 否则影子和草会各走各的。
 * 调用方负责给 a_InstanceMatrix0..3、a_InstanceParams 这组实例属性。 */

/* 距离抽稀的三个档位，由 GrassDraw::MakeDrawParams 逐片写入 */
uniform float u_BladeFullDistance = 20.0f;
uniform float u_BladeFadeDistance = 80.0f;
uniform float u_BladeMinDensity = 0.1f;

uniform vec2 u_WindDirection = vec2(1.0f, 0.35f); // @property
uniform float u_WindStrength = 0.12f;              // @property
uniform float u_WindSpeed = 1.6f;                  // @property
uniform float u_WindSpatialScale = 0.25f;          // @property
/* 时间由提交时逐片写入，不进材质面板 */
uniform float u_WindTime = 0.0f;
uniform float u_BladeCurl = 0.55f;                 // @property

/* 脚印图：rg 是倒向、b 是压下的程度；xy 是图左下角的世界坐标，zw 是 1 / 覆盖尺寸 */
uniform sampler2D u_TrampleMap;
uniform vec4 u_TrampleRect = vec4(0.0f);
uniform float u_TrampleStrength = 0.7f;            // @property

const float kGrassTwoPi = 6.2831853f;

/* 风场：沿风向推进的波 + 低频阵风包络。
 * 相位取世界坐标而不是每株随机，整片草才会像同一阵风扫过去；
 * 每株的抖动只作为一点点毛边，由调用方叠加。
 * wind_time 由调用方给：可见着色传当前时间，运动矢量还要再传一次上一帧的。 */
float GrassWind(vec2 world_xz, float wind_time, out vec2 wind_dir)
{
	wind_dir = normalize(u_WindDirection + vec2(1e-5f));
	const float along = dot(world_xz, wind_dir);
	const float phase = along * u_WindSpatialScale - wind_time * u_WindSpeed;
	const float wave = sin(phase);
	const float gust = sin(phase * 0.21f + wind_time * 0.37f) * 0.5f + 0.5f;
	return wave * mix(0.45f, 1.0f, gust);
}

/* 这一株是否已经稀到该丢：逐株判定，块内密度才连续 */
bool GrassBladeFaded(vec3 base, vec4 params)
{
	const float distance = length(u_ViewPos - base);
	return GrassDensityKeep(distance, GrassDensityDither(params),
		u_BladeFullDistance, u_BladeFadeDistance, u_BladeMinDensity) < 0.5f;
}

/* 把一个模板顶点（x ∈ [-1,1] 叶宽、y ∈ [0,1] 根到梢）展开成世界坐标。
 * base 是叶根、right/up/face 已经带好缩放；params 是实例参数（z 相位、w 色彩种子）。 */
vec3 ExpandGrassBlade(vec3 template_pos, vec3 base, vec3 right, vec3 up, vec3 face,
	vec4 params, float wind_time, out vec3 blade_normal)
{
	const float height = max(length(up), 1e-4f);
	const float t = template_pos.y;

	/* 叶宽从根到梢收窄 */
	const float taper = mix(1.0f, 0.08f, t * t);
	vec3 pos = base + right * (template_pos.x * 0.5f * taper) + up * t;

	/* 自然弯垂：越靠梢越往叶面方向倒，每株倒的量略有不同 */
	pos += face * (t * t) * (u_BladeCurl * height * (0.5f + params.w));

	/* 风场推力：只有梢部动，根部钉在地上 */
	vec2 wind_dir;
	const float wind = GrassWind(base.xz, wind_time, wind_dir) * u_WindStrength;
	const float flutter = sin(wind_time * u_WindSpeed * 4.7f + params.z * kGrassTwoPi)
		* 0.12f * u_WindStrength;
	const float bend = (wind + flutter) * t * t;
	pos += vec3(wind_dir.x, 0.0f, wind_dir.y) * bend;
	pos.y -= abs(bend) * 0.3f;

	/* 压弯：物体踩过的地方朝外倒，越靠近中心压得越平 */
	const vec2 trample_uv = (base.xz - u_TrampleRect.xy) * u_TrampleRect.zw;
	vec4 trample = vec4(0.5f, 0.5f, 0.0f, 0.0f);
	if (all(greaterThanEqual(trample_uv, vec2(0.0f))) && all(lessThanEqual(trample_uv, vec2(1.0f))))
		trample = texture(u_TrampleMap, trample_uv);

	const float press_scale = u_TrampleStrength * t * t * height;
	pos.xz += (trample.rg * 2.0f - 1.0f) * trample.b * press_scale;
	pos.y -= trample.b * press_scale * 0.8f;

	/* 法线整体朝上偏：用叶片自身法线会让整片草像刺猬，光照碎成一片 */
	blade_normal = normalize(mix(normalize(face), vec3(0.0f, 1.0f, 0.0f), 0.65f));
	return pos;
}

#endif // GRASS_BLADE_GLSL
