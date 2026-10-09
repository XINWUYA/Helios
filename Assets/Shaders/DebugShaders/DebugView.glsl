#type vertex
#version 410 core

/* 调试合成（全屏三角形）：顶点约定与 DeferredShaders/Lighting.glsl 相同 */
struct SVextex2Frag
{
	vec2 TexCoord;
};

layout(location = 0) in vec4 a_Position;
layout(location = 0) out SVextex2Frag vert2frag;

void main()
{
	/* ZO + Reversed-Z：全屏三角形 z=1 即近平面，恒通过 GEqual 比较 */
	gl_Position = a_Position;

	vert2frag.TexCoord = a_Position.xy * 0.5f + 0.5f;
}


#type fragment
#version 410 core

#include "../DeferredShaders/GBufferCommon.glsl"

struct SVextex2Frag
{
	vec2 TexCoord;
};

layout(location = 0) in SVextex2Frag vert2frag;
layout(location = 0) out vec4 OutFragColor;

/* Overdraw 计数纹理（DebugOverdraw.glsl 的加法累加结果）：
 * 只在 Overdraw 档有效，其余档位由 CPU 补黑 —— 声明的采样器每次绘制都必须有绑定。 */
layout(binding = 6) uniform sampler2D u_OverdrawTexture;

/* 调试档位：与 C++ DebugViewMode 序号一致（见 Kernel/src/Helios/Renderer/RenderCommon.h）。
 * 本 shader 处理 Surface 档（延迟：G-Buffer 直读）与 Overdraw 热力图。 */
uniform int u_DebugMode = 0;

const int kDebugAlbedo = 1;
const int kDebugNormal = 2;
const int kDebugRoughness = 3;
const int kDebugMetallic = 4;
const int kDebugSpecularColor = 5;
const int kDebugAmbientOcclusion = 6;
const int kDebugEmission = 7;
const int kDebugAmbient = 8;
const int kDebugOverdraw = 13;

/* Overdraw 热力色：0（背景）近黑 → 绿 → 红（高重叠）。
 * 计数步进 0.1（见 DebugOverdraw.glsl）→ 10 层饱和到红。 */
vec3 OverdrawHeat(float t)
{
	t = clamp(t, 0.0f, 1.0f);
	const vec3 cold = vec3(0.05f, 0.05f, 0.08f);
	const vec3 mid = vec3(0.10f, 0.85f, 0.20f);
	const vec3 hot = vec3(1.00f, 0.10f, 0.05f);
	return t < 0.5f ? mix(cold, mid, t * 2.0f) : mix(mid, hot, (t - 0.5f) * 2.0f);
}

void main()
{
	SGBufferData gbuffer;
	CalculateGBuffer(gbuffer, vert2frag.TexCoord);

	vec3 debug_color;
	if (u_DebugMode == kDebugAlbedo)
		debug_color = gbuffer.Albedo;
	else if (u_DebugMode == kDebugNormal)
		debug_color = normalize(gbuffer.WorldNormal) * 0.5f + 0.5f; /* 可视化重映射 */
	else if (u_DebugMode == kDebugRoughness)
		debug_color = vec3(gbuffer.Roughness);
	else if (u_DebugMode == kDebugMetallic)
		debug_color = vec3(gbuffer.Metallic);
	else if (u_DebugMode == kDebugSpecularColor)
		debug_color = gbuffer.Specular;
	else if (u_DebugMode == kDebugAmbientOcclusion)
		debug_color = vec3(gbuffer.AO);
	else if (u_DebugMode == kDebugEmission)
		debug_color = gbuffer.Emission;
	else if (u_DebugMode == kDebugAmbient)
		debug_color = gbuffer.Ambient;
	else
		/* kDebugOverdraw（其余档位不会发到这里）：覆盖全部像素 —— 背景像素计 0、
		 * 由天空（延迟）或清屏色（前向）在后收尾 */
		debug_color = OverdrawHeat(texture(u_OverdrawTexture, vert2frag.TexCoord).r);

	OutFragColor = vec4(debug_color, 1.0f);
}
