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

/* 屏幕坐标（NDC）按屏幕线性插值，逐像素解析反解视线方向。
 * 不拿插值后的顶点位置当方向：天空球是低模，三角形之间的方向只是一阶连续，
 * 银河这种大尺度平滑噪声会把那一丁点折角放大成一条条直线接缝 */
noperspective out vec2 v_Ndc;

void main()
{
	/* 与 SkyBox.glsl 同一套折叠：只吃视图旋转（天空不跟相机平移），
	 * 深度恒为远平面（Reversed-Z 下是 0），天空始终在最远处 */
	mat4 view_rotation = mat4(mat3(u_ViewMat));
	vec4 position = u_ProjectionMat * view_rotation * vec4(a_Position, 1.0f);
	gl_Position = vec4(position.xy, 0.0f, position.w);

	v_Ndc = position.xy / position.w;
}


#type fragment
#version 410 core

#include "../Builtin/Math.glsl"
#include "../Builtin/Uniforms.glsl"
#include "../Builtin/SkyUniforms.glsl"

layout(location = 0) out vec4 OutFragColor;

noperspective in vec2 v_Ndc;

/* 由 NDC 反解世界空间的视线方向：透视投影的 00 / 11 项就是
 * 1 / (纵横比 * tan(半视场)) 与 1 / tan(半视场)，除回去回到视空间斜率，
 * 再乘视图旋转的转置回到世界空间。
 * 正交投影下所有视线平行，这个式子退化成随像素发散的假射线，
 * 直接用正前方（正交视口里天空本来也没有什么可看的细节） */
vec3 ViewDirection(vec2 ndc)
{
	const bool orthographic = u_ProjectionMat[3][3] > 0.5f;
	if (orthographic)
		return normalize(transpose(mat3(u_ViewMat)) * vec3(0.0f, 0.0f, -1.0f));

	const vec3 view_ray = vec3(ndc.x / u_ProjectionMat[0][0], ndc.y / u_ProjectionMat[1][1], -1.0f);
	return normalize(transpose(mat3(u_ViewMat)) * view_ray);
}

/* 逐像素积分段数：视线 16 段 × 光源方向 8 段，档位决定地平线附近的色带精度。
 * 日月各跑一遍，白天只有太阳这遍跑（夜里才补月亮），被几何挡住的像素在深度测试就丢了 */
const int kViewSamples = 16;
const int kLightSamples = 8;

/* 太阳 / 月亮盘亮度增益：天上最亮的一小块，压到色调映射的白点上。
 * 月亮低一档：整个盘面顶到饱和就看不出月海与明暗界线，宁可少亮一点也不当白饼 */
const float kSunDiskGain = 6.0f;
const float kMoonDiskGain = 1.5f;

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

/* 某点沿光源方向到大气顶的光学厚度，瑞利与米氏分开放。
 * 光源被地球挡住时给个极大值，散射自然归零 —— 昼夜分界就靠这条。
 * 太阳和月亮共用：月亮反射的也是阳光，只是弱得多 */
vec2 LightOpticalDepth(vec3 position, vec3 light_dir)
{
	if (RaySphereNear(position, light_dir, u_SkyPlanet.x) > 0.0f)
		return vec2(1.0e9f);

	float distance = RaySphereFar(position, light_dir, u_SkyPlanet.y);
	if (distance <= 0.0f)
		return vec2(0.0f);

	float step = distance / float(kLightSamples);
	float depth_rayleigh = 0.0f;
	float depth_mie = 0.0f;
	for (int i = 0; i < kLightSamples; ++i)
	{
		vec3 sample_position = position + light_dir * (step * (float(i) + 0.5f));
		float height = max(length(sample_position) - u_SkyPlanet.x, 0.0f);
		depth_rayleigh += exp(-height / u_SkyRayleigh.w) * step;
		depth_mie += exp(-height / u_SkyMie.y) * step;
	}
	return vec2(depth_rayleigh, depth_mie);
}

/* 单个光源的单次散射：沿视线积分瑞利 + 米氏，返回散射辐亮度。
 * 视线透过率只跟视线有关，日月算出来是同一个值，取第一遍的就行 */
vec3 ScatterFromLight(vec3 view_dir, vec3 light_dir, vec3 light_radiance,
	out vec3 view_transmittance)
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

		/* 到光源的光学厚度要加上已经走过的这段，日光一路衰减进来 */
		vec2 light_depth = LightOpticalDepth(position, light_dir);
		vec3 tau = beta_rayleigh * (depth_rayleigh + light_depth.x)
			+ vec3(beta_mie) * (depth_mie + light_depth.y);
		vec3 attenuation = exp(-tau);

		sum_rayleigh += density_rayleigh * attenuation * step;
		sum_mie += density_mie * attenuation * step;
	}

	float mu = dot(view_dir, light_dir);
	float phase_rayleigh = 3.0f / (16.0f * PI) * (1.0f + mu * mu);
	float phase_mie = 3.0f / (8.0f * PI) * ((1.0f - g * g) * (1.0f + mu * mu))
		/ ((2.0f + g * g) * pow(1.0f + g * g - 2.0f * g * mu, 1.5f));

	view_transmittance = exp(-(beta_rayleigh * depth_rayleigh + vec3(beta_mie) * depth_mie));
	return light_radiance * (beta_rayleigh * phase_rayleigh * sum_rayleigh
		+ vec3(beta_mie * phase_mie) * sum_mie);
}

float Hash21(vec2 p)
{
	p = fract(p * vec2(123.34f, 456.21f));
	p += dot(p, p + 45.32f);
	return fract(p.x * p.y);
}

float Hash31(vec3 p)
{
	p = fract(p * vec3(0.1031f, 0.1030f, 0.0973f));
	p += dot(p, p.yzx + 33.33f);
	return fract((p.x + p.y) * p.z);
}

/* 三维值噪声：银河的云块与尘埃暗带用它铺。只在银道带附近才真正跑起来
 * （带外调用方提前返回），所以大部分天空像素不付这份代价 */
float ValueNoise(vec3 p)
{
	const vec3 cell = floor(p);
	vec3 t = fract(p);
	t = t * t * (3.0f - 2.0f * t);

	const float n000 = Hash31(cell);
	const float n100 = Hash31(cell + vec3(1.0f, 0.0f, 0.0f));
	const float n010 = Hash31(cell + vec3(0.0f, 1.0f, 0.0f));
	const float n110 = Hash31(cell + vec3(1.0f, 1.0f, 0.0f));
	const float n001 = Hash31(cell + vec3(0.0f, 0.0f, 1.0f));
	const float n101 = Hash31(cell + vec3(1.0f, 0.0f, 1.0f));
	const float n011 = Hash31(cell + vec3(0.0f, 1.0f, 1.0f));
	const float n111 = Hash31(cell + vec3(1.0f, 1.0f, 1.0f));

	const float front = mix(mix(n000, n100, t.x), mix(n010, n110, t.x), t.y);
	const float back = mix(mix(n001, n101, t.x), mix(n011, n111, t.x), t.y);
	return mix(front, back, t.z);
}

float Fbm(vec3 p, int octaves)
{
	float sum = 0.0f;
	float amplitude = 0.5f;
	for (int i = 0; i < octaves; ++i)
	{
		sum += ValueNoise(p) * amplitude;
		p = p * 2.03f + vec3(17.3f, 5.1f, 9.7f);
		amplitude *= 0.5f;
	}
	return sum;
}

/* 方向在哪个立方体面上，以及面内坐标（弧度，到面中心的角度，最大 π/4）。
 * 星空按立方体的面切格点而不是按经纬度：经纬度把整个极点压成一条线，
 * 极区整片格点被拉成细条，星点会变成放射状的短线；六个面的大小一致，也没有退化点 */
int DirectionToFace(vec3 d, out vec2 arc)
{
	vec3 a = abs(d);
	vec2 st;
	int face;
	if (a.x >= a.y && a.x >= a.z)
	{
		st = vec2(d.z, d.y) / a.x;
		face = d.x >= 0.0f ? 0 : 1;
	}
	else if (a.y >= a.z)
	{
		st = vec2(d.x, d.z) / a.y;
		face = d.y >= 0.0f ? 2 : 3;
	}
	else
	{
		st = vec2(d.x, d.y) / a.z;
		face = d.z >= 0.0f ? 4 : 5;
	}

	/* 面内等距重排：按到面中心的角度铺开，格的面积在整面内几乎不变
	 * （直接拿 tan 投影的坐标切格，靠角上的格子会又小又密） */
	const float radius = length(st);
	const float angle = atan(radius);
	arc = radius > 1.0e-6f ? st * (angle / radius) : st;
	return face;
}

/* 面内坐标反解回方向：与 DirectionToFace 同一套约定 */
vec3 FaceToDirection(int face, vec2 arc)
{
	const float radius = length(arc);
	const float scale = radius > 1.0e-6f ? tan(radius) / radius : 1.0f;
	const vec2 st = arc * scale;

	vec3 dir;
	if (face == 0) dir = vec3(1.0f, st.y, st.x);
	else if (face == 1) dir = vec3(-1.0f, st.y, st.x);
	else if (face == 2) dir = vec3(st.x, 1.0f, st.y);
	else if (face == 3) dir = vec3(st.x, -1.0f, st.y);
	else if (face == 4) dir = vec3(st.x, st.y, 1.0f);
	else dir = vec3(st.x, st.y, -1.0f);
	return normalize(dir);
}

/* 星等换成相对流量：每暗一个星等流量降到 1 / 2.512。1 等星记作 1.0，
 * 天狼星（-1.46 等）约 9.6，肉眼极限（6.5 等）约 0.0063。
 * 这一步是"假"与"真"的分界：等亮度撒点看着像噪点，按星等铺开才像星空 */
float MagnitudeToFlux(float magnitude)
{
	return pow(10.0f, -0.4f * (magnitude - 1.0f));
}

/* 恒星色温：真实夜空里亮星的颜色差别很明显（参宿四橙红、织女蓝白），
 * 全画成白色是另一个"假"的来源。0 冷红 → 1 蓝白 */
vec3 StarTint(float temperature)
{
	const vec3 orange = vec3(1.00f, 0.58f, 0.32f);
	const vec3 amber = vec3(1.00f, 0.76f, 0.55f);
	const vec3 yellow = vec3(1.00f, 0.91f, 0.76f);
	const vec3 white = vec3(1.00f, 0.98f, 0.97f);
	const vec3 blue = vec3(0.72f, 0.82f, 1.00f);

	if (temperature < 0.35f)
		return mix(orange, amber, temperature / 0.35f);
	if (temperature < 0.62f)
		return mix(amber, yellow, (temperature - 0.35f) / 0.27f);
	if (temperature < 0.84f)
		return mix(yellow, white, (temperature - 0.62f) / 0.22f);
	return mix(white, blue, (temperature - 0.84f) / 0.16f);
}

/* 星点的点扩散：一个核加一圈很淡的光晕。宽度固定按像素算 —— 恒星是点光源，
 * 成像尺寸由光学决定、与亮度无关；亮星看着更大是因为外围光晕越过了可见阈值。
 * 核半径不随分辨率变，低分辨率下也不会被采样漏掉（整片星空一闪一闪地消失） */
float StarProfile(float angular_distance, float pixel_angle)
{
	/* 单位向量的差就是两方向夹角的弦长，小角下与角度同量级，省一次 acos */
	const float radius = angular_distance / max(pixel_angle, 1.0e-9f);
	const float sigma = 0.62f;
	const float core = exp(-0.5f * radius * radius / (sigma * sigma));
	const float halo = 0.05f * exp(-radius / (sigma * 3.0f));
	return core + halo;
}

/* 一层星点：格点里每格至多一颗，位置 / 星等 / 色温都由格点哈希决定 */
vec3 StarLayer(vec3 d, int face, vec2 arc, float cells, float seed, float density,
	float magnitude_min, float magnitude_max, float pixel_angle)
{
	const vec2 uv = arc * cells;
	const vec2 cell = floor(uv);
	const float face_seed = seed + float(face) * 37.0f;

	const float presence = Hash21(cell + face_seed);
	if (presence > density)
		return vec3(0.0f);

	const vec2 jitter = vec2(Hash21(cell + face_seed + 7.1f), Hash21(cell + face_seed + 13.7f));
	const vec3 star_direction = FaceToDirection(face, (cell + jitter) / cells);

	/* 星等取幂次分布：暗星远多于亮星，肉眼看到的天空绝大多数星点都挤在极限星等附近 */
	const float rank = Hash21(cell + face_seed + 3.7f);
	const float magnitude = mix(magnitude_min, magnitude_max, pow(rank, 0.35f));

	/* 色温同样偏冷：M 型星占多数，蓝白星稀少 */
	const float temperature = pow(Hash21(cell + face_seed + 21.3f), 1.5f);

	const float flux = MagnitudeToFlux(magnitude);
	const vec3 offset = d - star_direction;
	return StarTint(temperature) * (flux * StarProfile(length(offset), pixel_angle));
}

/* 银河：一条环绕整片天球的亮带。银极与天极的夹角固定 62.87°、方位取一个固定约定，
 * 于是整条带跟着星空一起转（转角已经算进 d 里）。带内是成千上万颗分辨不出的恒星
 * 叠出来的漫射光，所以星点密度也一起提上去，银河才"看得出颗粒"。
 * band 是到银道面的权重，留给星点层做密度调制 */
vec3 MilkyWay(vec3 d, out float band)
{
	const vec3 celestial_pole = u_SkyStars.xyz;
	const vec3 reference = abs(celestial_pole.y) < 0.99f ? vec3(0.0f, 1.0f, 0.0f)
		: vec3(1.0f, 0.0f, 0.0f);
	const vec3 axis = normalize(cross(reference, celestial_pole));
	const float tilt = 1.0973f; /* 62.87° */
	const vec3 galactic_pole = normalize(celestial_pole * cos(tilt) + axis * sin(tilt));

	/* 银纬的正弦，以及沿着带绕一圈的位置：银心那一侧更亮更宽，
	 * 整条带才不会是一条等宽等亮的几何带子 */
	const float latitude = dot(d, galactic_pole);
	const vec3 in_plane_a = normalize(cross(galactic_pole, reference));
	const vec3 in_plane_b = cross(galactic_pole, in_plane_a);
	const float along = atan(dot(d, in_plane_b), dot(d, in_plane_a));
	const float bulge = 0.55f + 0.45f * cos(along);
	const float width = 0.11f + 0.05f * bulge;

	band = exp(-latitude * latitude / (width * width));
	if (band < 0.02f)
		return vec3(0.0f);

	/* 云块与尘埃：大尺度亮暗不均，横在中间的暗缝就是"银河裂缝" */
	const float clouds = Fbm(d * 5.4f, 3);
	const float rift = smoothstep(0.42f, 0.72f, Fbm(d * 9.0f + vec3(4.7f, 1.3f, 8.9f), 2));
	const float shape = band * (0.55f + 0.90f * clouds) * (1.0f - 0.65f * rift) * (0.65f + 0.45f * bulge);

	/* 偏暖的乳白色，比气辉亮一档：亮度要压得住，它是背景不是主体，
	 * 太亮就成了一片糊在天上的云 */
	return vec3(0.95f, 0.92f, 0.86f) * (shape * 0.07f);
}

/* 星空：把视线反向转回星空静止的坐标里取样，于是整片星空绕极轴转。
 * 三层格点对应三档亮度：极密的暗星铺底、主序层、数量很少的亮星层。
 * cells 是每弧度的格数（104 → 0.55° 一格） */
vec3 Stars(vec3 direction, float night)
{
	vec3 d = RotateAroundAxis(direction, u_SkyStars.xyz, u_SkyStars.w);

	vec2 arc;
	const int face = DirectionToFace(d, arc);

	/* 一像素折算成多少弧度。取两条边的较大值：星点尺寸按像素给，
	 * 换分辨率时星点看着一样大，不会在高分辨率下缩成一个亚像素点 */
	const float pixel_angle = max(length(dFdx(d)), length(dFdy(d)));

	float band = 0.0f;
	const vec3 milky_way = MilkyWay(d, band);
	const float density_scale = 1.0f + 4.0f * band;

	vec3 stars = StarLayer(d, face, arc, 104.0f, 3.0f, 0.16f * density_scale,
		4.2f, 6.8f, pixel_angle);
	stars += StarLayer(d, face, arc, 34.0f, 17.0f, 0.05f * density_scale,
		2.0f, 5.2f, pixel_angle);
	stars += StarLayer(d, face, arc, 11.0f, 29.0f, 0.10f * density_scale,
		-1.4f, 2.4f, pixel_angle);

	return (stars * 1.5f + milky_way) * (u_SkyPlanet.z * night);
}

/* 盘面基底：tangent 水平、bitangent 竖直。贴近天顶时「竖直」没有定义
 * （极轴与视线重合），随便取一个水平参考即可 —— 那个高度角的压扁因子本来就是 1 */
void DiscBasis(vec3 disc_dir, out vec3 tangent, out vec3 bitangent)
{
	const vec3 reference = abs(disc_dir.y) < 0.98f ? vec3(0.0f, 1.0f, 0.0f) : vec3(1.0f, 0.0f, 0.0f);
	tangent = normalize(cross(reference, disc_dir));
	bitangent = cross(disc_dir, tangent);
}

/* 盘面局部坐标：x 水平、y 竖直，单位是盘半径。
 * 竖直方向除以压扁因子 —— 大气折射把盘的上下边缘抬得不一样多，贴地平线时
 * 日面月亮都是扁的椭圆，高度角十度以上才近似圆 */
vec2 DiscLocal(vec3 view_dir, vec3 disc_dir, vec3 tangent, vec3 bitangent,
	float angular_radius, float flattening)
{
	const vec3 offset = view_dir - disc_dir * dot(view_dir, disc_dir);
	return vec2(dot(offset, tangent), dot(offset, bitangent) / max(flattening, 1.0e-3f))
		/ angular_radius;
}

/* 月亮：盘内逐像素重建球面法线，再按太阳方向着色 —— 月相、明暗界线朝向都是算出来的，
 * 不用摆一张贴图也不用按月相切换材质。
 * 月面本身并不均匀：暗色的月海集中在朝向地球这一面的几片，缺了它月亮就是一块
 * 打了光的高光斑，只有形状没有质感 */
vec3 Moon(vec3 view_dir, float pixel_angle, float flattening)
{
	const vec3 moon_dir = u_SkyMoonDirection.xyz;
	const float angular_radius = max(u_SkyMoonDirection.w, 1.0e-5f);
	const float cos_angle = dot(view_dir, moon_dir);
	if (cos_angle < cos(angular_radius * 1.3f))
		return vec3(0.0f);

	vec3 tangent;
	vec3 bitangent;
	DiscBasis(moon_dir, tangent, bitangent);

	const vec2 local = DiscLocal(view_dir, moon_dir, tangent, bitangent, angular_radius, flattening);
	const float radius = length(local);

	/* 边缘按像素宽度收：月面轮廓是硬边，按盘面比例做淡出会随分辨率越糊。
	 * 上限 0.22：盘面只有几像素时按 1.5 像素折算会把整个边缘都吃掉 */
	const float edge_width = clamp(1.5f * pixel_angle / angular_radius, 0.02f, 0.22f);
	const float edge = 1.0f - smoothstep(1.0f - edge_width, 1.0f, radius);
	if (edge <= 0.0f)
		return vec3(0.0f);

	/* 朝地球这一面的法线：沿 -moon_dir 抬起来再叠切平面偏移 */
	const vec3 normal = normalize(-moon_dir * sqrt(max(1.0f - radius * radius, 0.0f))
		+ tangent * local.x + bitangent * local.y);
	const float lit = smoothstep(-0.05f, 0.12f, dot(normal, u_SkyMoonLightDirection.xyz));

	/* 月海：几片低反射率的大块（尺度取盘面的三分之一左右，太小就成了麻点），
	 * 叠几层噪声当环形山造成的明暗 */
	const float maria = smoothstep(0.38f, 0.62f, Fbm(vec3(local * 1.55f, 1.7f), 3));
	const float albedo = (0.52f + 0.48f * maria) * (1.0f - 0.06f * radius);

	/* 地照：背光面被地球反射的日光微微照亮，新月前后能看到整个圆面 */
	const float earthshine = 0.03f * (1.0f - lit);
	return u_SkyMoonRadiance.rgb * (kMoonDiskGain * edge * (lit * albedo + earthshine));
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
	vec3 view_dir = ViewDirection(v_Ndc);
	vec3 origin = vec3(0.0f, u_SkyPlanet.x, 0.0f);
	bool hits_ground = RaySphereNear(origin, view_dir, u_SkyPlanet.x) > 0.0f;

	vec3 transmittance;
	vec3 radiance = ScatterFromLight(view_dir, u_SkySunDirection.xyz, u_SkySunRadiance.rgb,
		transmittance);

	float sun_elevation = u_SkySunDirection.xyz.y;
	float night = 1.0f - smoothstep(-0.08f, 0.12f, sun_elevation);

	/* 月亮同样在散射。满月只有太阳的几十分之一，但夜空本来就暗，
	 * 月亮附近会浮出一圈辉光；白天这遍跳过，省一半积分 */
	if (night > 0.0f)
	{
		vec3 moon_transmittance;
		radiance += ScatterFromLight(view_dir, u_SkyMoonDirection.xyz, u_SkyMoonRadiance.rgb,
			moon_transmittance) * night;
	}

	/* 一像素折算成多少弧度：日面边缘、月面边缘都按像素宽度给，
	 * 换分辨率时它们的锐利程度才不会跟着变 */
	const float pixel_angle = max(length(dFdx(view_dir)), length(dFdy(view_dir)));

	if (!hits_ground)
	{
		/* 盘面按角度比较：cos 空间里做 smoothstep 会把边缘放大成几十度的光斑。
		 * 竖直方向按折射压扁，贴地平线时日面是扁的椭圆 */
		const float sun_radius = max(u_SkySunDirection.w, 1.0e-5f);
		vec3 sun_tangent;
		vec3 sun_bitangent;
		DiscBasis(u_SkySunDirection.xyz, sun_tangent, sun_bitangent);
		const float sun_ratio = length(DiscLocal(view_dir, u_SkySunDirection.xyz, sun_tangent,
			sun_bitangent, sun_radius, u_SkySunRadiance.w));
		const float disk = 1.0f - smoothstep(0.90f, 1.0f, sun_ratio);
		/* 边缘变暗：视线在日面边缘穿过的光球层更浅，看得到的高度更低、温度更低，
		 * 所以边缘比中心暗一档 —— 日面不是一块均匀的白斑 */
		const float limb = 1.0f - 0.55f * (1.0f - sqrt(max(1.0f - sun_ratio * sun_ratio, 0.0f)));
		/* 盘外的晕：眼球与光学系统在强光周围的那圈散射，日面才不会像贴上去的一个圆片 */
		const float glow = 0.08f * exp(-max(sun_ratio - 1.0f, 0.0f) * 2.2f);
		radiance += u_SkySunRadiance.rgb * transmittance * (disk * limb * kSunDiskGain + glow);

		/* 盘外那圈月辉：月光被气溶胶小角度前向散射，再加上眼球与镜头里的散射，
		 * 尺度比日面那圈大得多（日面太亮，晕只在零点几度里看得出来）。
		 * 单次散射补不出这段：米氏前向瓣在两三度的范围里几乎是平的，
		 * 少了这一项满月就是一块白盘贴在黑底上 */
		const float moon_radius = max(u_SkyMoonDirection.w, 1.0e-5f);
		const float moon_angle = acos(clamp(dot(view_dir, u_SkyMoonDirection.xyz), -1.0f, 1.0f));
		const float moon_ratio = moon_angle / moon_radius;
		/* 从盘缘起才起量：盘面本身已经顶到自己的亮度，再垫一层会把它压灰、
		 * 月海的反差也一起塌掉 */
		const float moon_glow = 0.12f * exp(-max(moon_ratio - 1.0f, 0.0f) * 0.21f)
			* smoothstep(0.90f, 1.10f, moon_ratio);

		radiance += Moon(view_dir, pixel_angle, u_SkyMoonRadiance.w) * transmittance;
		radiance += u_SkyMoonRadiance.rgb * transmittance * moon_glow;
		/* 星星同样走大气透过率：贴近地平线时被吃掉一大半并偏红，与日月同一套消光 */
		radiance += Stars(view_dir, night) * transmittance;
		radiance += kNightAirglow * night;
	}
	else
	{
		/* 视线打到地面：压暗，空场景的下半球不至于也挂满星星 */
		radiance *= 0.25f;
	}

	OutFragColor = vec4(ToneMap(radiance * u_SkyPlanet.w), 1.0f);
}
