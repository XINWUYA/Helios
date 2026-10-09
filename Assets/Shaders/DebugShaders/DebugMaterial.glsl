#type vertex
#version 410 core

/* 调试材质（几何重绘）：Mipmap 档（两管线）+ 前向的 Surface 档。
 * 顶点属性按引擎统一契约（location 0..4 = 位置 / 法线 / 颜色 / UV / 切线）。 */
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec4 a_Color;
layout(location = 3) in vec3 a_TexCoord;
layout(location = 4) in vec3 a_Tangent;

#include "../builtin/Uniforms.glsl"

struct SVextex2Frag
{
	vec4 BaseColor;
	vec2 TexCoord;
	vec3 WorldNormal;
	vec3 WorldTangent;
};

layout(location = 0) out SVextex2Frag vert2frag;

void main()
{
	gl_Position = u_ViewProjectionMat * u_Local2WorldMat * vec4(a_Position, 1.0f);

	vert2frag.BaseColor = a_Color;
	vert2frag.TexCoord = a_TexCoord.xy;

	mat3 normal_mat = transpose(inverse(mat3(u_Local2WorldMat)));
	vert2frag.WorldNormal = normalize(normal_mat * a_Normal);
	vert2frag.WorldTangent = normalize(normal_mat * a_Tangent);
}


#type fragment
#version 410 core

#include "../builtin/Math.glsl"

struct SVextex2Frag
{
	vec4 BaseColor;
	vec2 TexCoord;
	vec3 WorldNormal;
	vec3 WorldTangent;
};

layout(location = 0) in SVextex2Frag vert2frag;
layout(location = 0) out vec4 OutFragColor;

/* 主贴图：按调试档位在 CPU 侧从源材质选定的那张贴图（缺省补白）。
 * UV 变换与源材质同源（u_AlbedoTilingOffset，缺省不变换）。 */
layout(binding = 0) uniform sampler2D u_MainTexture;
layout(location = 0) uniform vec4 u_AlbedoTilingOffset = vec4(1.0f, 1.0f, 0.0f, 0.0f);

/* 调试档位：与 C++ DebugViewMode 序号一致（见 Kernel/src/Helios/Renderer/RenderCommon.h）。
 * 本 shader 处理 Mipmap（两条管线）与前向的 Surface 档；延迟的 Surface 档走
 * G-Buffer 直读（DebugView.glsl），不经这里。 */
uniform int u_DebugMode = 0;

const int kDebugAlbedo = 1;
const int kDebugNormal = 2;
const int kDebugRoughness = 3;
const int kDebugMetallic = 4;
const int kDebugSpecularColor = 5;
const int kDebugAmbientOcclusion = 6;
const int kDebugEmission = 7;
const int kDebugAmbient = 8;
const int kDebugMipmap = 14;

/* Mip 层级热力色：mip 0（最清晰）红 → 黄 → 绿 → 蓝（最糊）。t = 采样层级 / 最大层级 */
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

	if (u_DebugMode == kDebugAlbedo)
	{
		/* 与 PBRStandard 的 Albedo 输入同口径（线性化 × 顶点色） */
		OutFragColor = vec4(SrgbToLinear(texture(u_MainTexture, base_uv).rgb) * vert2frag.BaseColor.rgb, 1.0f);
		return;
	}
	if (u_DebugMode == kDebugNormal)
	{
		vec3 T = normalize(vert2frag.WorldTangent);
		vec3 N = normalize(vert2frag.WorldNormal);
		T = normalize(T - dot(T, N) * N);
		mat3 TBN = mat3(T, cross(T, N), N);
		const vec3 normal = texture(u_MainTexture, base_uv).xyz * 2.0f - 1.0f;
		OutFragColor = vec4(normalize(TBN * normal) * 0.5f + 0.5f, 1.0f);
		return;
	}
	if (u_DebugMode == kDebugRoughness || u_DebugMode == kDebugMetallic)
	{
		OutFragColor = vec4(vec3(texture(u_MainTexture, base_uv).r), 1.0f);
		return;
	}
	if (u_DebugMode == kDebugAmbientOcclusion)
	{
		/* 材质体系里 AO 恒为 1（与 GBufferMaterial 的写入约定一致） */
		OutFragColor = vec4(1.0f, 1.0f, 1.0f, 1.0f);
		return;
	}

	/* SpecularColor / Emission / Ambient：直接取对应贴图的 RGB */
	OutFragColor = vec4(texture(u_MainTexture, base_uv).rgb, 1.0f);
}
