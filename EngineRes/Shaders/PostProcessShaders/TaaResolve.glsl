#type vertex
#version 410 core

/* 全屏三角形：z=1 即近平面（Reversed-Z），不测深度 */
layout(location = 0) in vec4 a_Position;

void main()
{
	gl_Position = a_Position;
}


#type fragment
#version 410 core

/* 时域抗锯齿的解析：
 *   当前帧色 + 上一帧历史（按运动矢量重投影）→ 邻域方差裁剪 → 混合 → 轻锐化
 * 运动矢量由场景 / G-Buffer 的 Velocity 附件给，坐标是 UV 单位；
 * 深度用来回投影和判物体边界，天空（远平面）不累积。 */

uniform sampler2D u_SceneColor;
uniform sampler2D u_Depth;
uniform sampler2D u_Velocity;
uniform sampler2D u_History;

/* 当前帧视图投影的逆（含抖动）：深度反投影回世界用 */
uniform mat4 u_InvViewProjection;
uniform vec2 u_TexelSize;
uniform vec2 u_TextureSize;
uniform float u_HistoryWeight; /* 静止时历史占比 */
uniform float u_Sharpness;
uniform int u_HistoryValid;

layout(location = 0) out vec4 OutColor;

/* YCoCg 里做裁剪：三个通道相关性高，比直接裁 RGB 少出彩边 */
vec3 ToYCoCg(vec3 color)
{
	const float y = dot(color, vec3(0.25f, 0.5f, 0.25f));
	const float co = dot(color, vec3(0.5f, 0.0f, -0.5f));
	const float cg = dot(color, vec3(-0.25f, 0.5f, -0.25f));
	return vec3(y, co, cg);
}

vec3 FromYCoCg(vec3 color)
{
	const float r = color.x + color.y - color.z;
	const float g = color.x + color.z;
	const float b = color.x - color.y - color.z;
	return vec3(r, g, b);
}

void main()
{
	const vec2 uv = gl_FragCoord.xy * u_TexelSize;

	/* 场景色按 [0,1] 截断：TAA 关掉时 8bit 附件本来就是这个效果，保持像素基线一致 */
	const vec3 current = min(texture(u_SceneColor, uv).rgb, vec3(1.0f));

	const float depth = texture(u_Depth, uv).r;
	const vec2 velocity = texture(u_Velocity, uv).rg;

	/* 3x3 邻域：均值与方差（历史裁剪用） */
	vec3 sum = vec3(0.0f);
	vec3 sum_sq = vec3(0.0f);
	for (int y = -1; y <= 1; ++y)
	{
		for (int x = -1; x <= 1; ++x)
		{
			const vec2 offset = vec2(float(x), float(y)) * u_TexelSize;
			const vec3 neighbor = min(texture(u_SceneColor, uv + offset).rgb, vec3(1.0f));
			const vec3 yc = ToYCoCg(neighbor);
			sum += yc;
			sum_sq += yc * yc;
		}
	}
	const vec3 mean = sum / 9.0f;
	const vec3 sigma = sqrt(max(sum_sq / 9.0f - mean * mean, vec3(0.0f)));

	/* 远平面（Reversed-Z 下是 0）就是天空：内容随相机转，累积只会拖一条尾巴 */
	const bool is_sky = depth <= 1.0e-7f;

	const vec2 prev_uv = uv - velocity;
	const bool outside = any(lessThan(prev_uv, vec2(0.0f))) || any(greaterThan(prev_uv, vec2(1.0f)));

	vec3 history = current;
	float alpha = 1.0f;

	if (u_HistoryValid != 0 && !outside && !is_sky)
	{
		vec3 history_yc = ToYCoCg(min(texture(u_History, prev_uv).rgb, vec3(1.0f)));

		/* 方差裁剪：历史落在当前邻域统计之外就拉回来，色块不会糊成一片 */
		history_yc = clamp(history_yc, mean - sigma, mean + sigma);

		/* 混合权重：静止多信历史；动得越快越信当前帧。
		 * 物体边界不靠深度另加权重 —— 那会让边缘永远收敛不了，
		 * 串色交给上面的方差裁剪兜（它本来就按邻域亮暗范围限制历史）。 */
		const float speed = length(velocity * u_TextureSize);
		const float current_weight = mix(1.0f - u_HistoryWeight, 0.35f, clamp(speed / 8.0f, 0.0f, 1.0f));

		history = FromYCoCg(history_yc);
		alpha = current_weight;
	}

	vec3 resolved = mix(history, current, alpha);

	/* 锐化：只补十字邻域与中心的差，再夹回邻域范围，避免补出新锯齿 */
	if (u_Sharpness > 0.0f)
	{
		const vec3 blur = (texture(u_SceneColor, uv + vec2(u_TexelSize.x, 0.0f)).rgb
			+ texture(u_SceneColor, uv - vec2(u_TexelSize.x, 0.0f)).rgb
			+ texture(u_SceneColor, uv + vec2(0.0f, u_TexelSize.y)).rgb
			+ texture(u_SceneColor, uv - vec2(0.0f, u_TexelSize.y)).rgb) * 0.25f;

		vec3 sharpened = ToYCoCg(resolved);
		sharpened += (ToYCoCg(current) - ToYCoCg(blur)) * u_Sharpness;
		sharpened = clamp(sharpened, mean - sigma, mean + sigma);
		resolved = FromYCoCg(sharpened);
	}

	OutColor = vec4(max(resolved, vec3(0.0f)), 1.0f);
}
