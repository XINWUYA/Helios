#type vertex
#version 410 core

/* 调试材质（几何重绘）：Mipmap 与 Overdraw 档（两管线），以及前向里没接调试分流的自定义
 * 着色器的兜底。顶点属性按引擎统一契约（location 0..4 = 位置 / 法线 / 颜色 / UV / 切线） */
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec4 a_Color;
layout(location = 3) in vec3 a_TexCoord;
layout(location = 4) in vec3 a_Tangent;

#include "../Builtin/Uniforms.glsl"

struct SVextex2Frag
{
	vec4 BaseColor;
	vec2 TexCoord;
	vec3 WorldNormal;
	vec3 WorldTangent;
};

out SVextex2Frag vert2frag;

void main()
{
	gl_Position = u_ViewProjectionMat * u_Local2WorldMat * vec4(a_Position, 1.0f);

	vert2frag.BaseColor = a_Color;
	vert2frag.TexCoord = a_TexCoord.xy;

	/* 法线矩阵：非等比缩放下法线 / 切线按逆转置矩阵变换 */
	mat3 normal_mat = transpose(inverse(mat3(u_Local2WorldMat)));
	vert2frag.WorldNormal = normalize(normal_mat * a_Normal);
	vert2frag.WorldTangent = normalize(normal_mat * a_Tangent);
}


#type fragment
#version 410 core

#include "../Builtin/Math.glsl"
#include "DebugViewCommon.glsl"

struct SVextex2Frag
{
	vec4 BaseColor;
	vec2 TexCoord;
	vec3 WorldNormal;
	vec3 WorldTangent;
};

in SVextex2Frag vert2frag;
layout(location = 0) out vec4 OutFragColor;

/* 主贴图：按调试档位在 CPU 侧从源材质选定的那张贴图（缺省补白）；
 * UV 变换与源材质同源（u_AlbedoTilingOffset，缺省不变换） */
uniform sampler2D u_MainTexture;
uniform vec4 u_AlbedoTilingOffset = vec4(1.0f, 1.0f, 0.0f, 0.0f);

uniform int u_DebugMode = 0;

/* Mip 层级热力色：mip 0（最清晰）红 → 黄 → 绿 → 蓝（最糊）；t = 采样层级 / 最大层级 */
vec3 MipLevelHeat(float t)
{
	t = clamp(t, 0.0f, 1.0f);
	if (t < 0.33f)
		return mix(vec3(0.90f, 0.15f, 0.10f), vec3(0.95f, 0.85f, 0.15f), t / 0.33f);
	if (t < 0.66f)
		return mix(vec3(0.95f, 0.85f, 0.15f), vec3(0.20f, 0.80f, 0.30f), (t - 0.33f) / 0.33f);
	return mix(vec3(0.20f, 0.80f, 0.30f), vec3(0.20f, 0.40f, 0.95f), (t - 0.66f) / 0.34f);
}

void main()
{
	/* 重叠计数：加法混合下每个片元累加一份步进，显示层交给 DebugView.glsl 的热力图 */
	if (u_DebugMode == kDebugOverdraw)
	{
		OutFragColor = vec4(kOverdrawStep);
		return;
	}

	/* 阴影累加的兜底：权重 0 读作全受光，覆盖标记照写（背景才会被保留） */
	if (u_DebugMode == kDebugShadow)
	{
		OutFragColor = vec4(0.0f, 0.0f, 2.0f, 0.0f);
		return;
	}

	vec2 base_uv = vert2frag.TexCoord * u_AlbedoTilingOffset.xy + u_AlbedoTilingOffset.zw;

	/* Mipmap 档：按屏幕空间采样足迹求 mip 层级（与硬件采样同口径），
	 * 底色 = 该层级实际采到的内容、叠加层级热力色 —— 既看"第几层"又看"糊成什么样" */
	if (u_DebugMode == kDebugMipmap)
	{
		const float lod = textureQueryLod(u_MainTexture, base_uv).x;
		const vec2 texture_size = vec2(textureSize(u_MainTexture, 0));
		const float max_lod = max(log2(max(texture_size.x, texture_size.y)), 1.0f);
		const vec3 level_color = textureLod(u_MainTexture, base_uv, lod).rgb;
		OutFragColor = vec4(mix(level_color, MipLevelHeat(lod / max_lod), 0.65f), 1.0f);
		return;
	}

	/* 光照分量：自定义着色器的光照算法引擎不知道，给不出读数就画黑 */
	if (!IsDebugSurfaceMode(u_DebugMode))
	{
		OutFragColor = vec4(0.0f, 0.0f, 0.0f, 1.0f);
		return;
	}

	/* Surface 档：只看 CPU 选定的那一张贴图，其余通道按贴图约定的缺省值 */
	const vec4 main_sample = texture(u_MainTexture, base_uv);
	vec3 T = normalize(vert2frag.WorldTangent);
	vec3 N = normalize(vert2frag.WorldNormal);
	T = normalize(T - dot(T, N) * N);
	const mat3 TBN = mat3(T, cross(T, N), N);

	SMaterialInput mtl = MakeDebugMaterialInput(SrgbToLinear(main_sample.rgb) * vert2frag.BaseColor.rgb,
		normalize(TBN * (main_sample.xyz * 2.0f - 1.0f)), main_sample.r, main_sample.r);
	mtl.Specular = main_sample.rgb;
	mtl.Emission = main_sample.rgb;
	mtl.Ambient = main_sample.rgb;
	OutFragColor = vec4(DebugSurfaceChannel(u_DebugMode, mtl), 1.0f);
}
