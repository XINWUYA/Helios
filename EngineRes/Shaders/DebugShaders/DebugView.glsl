#type vertex
#version 410 core

/* 调试合成（全屏三角形）：顶点约定与 DeferredShaders/Lighting.glsl 相同 */
struct SVextex2Frag
{
	vec2 TexCoord;
};

layout(location = 0) in vec4 a_Position;
out SVextex2Frag vert2frag;

void main()
{
	/* 全屏三角形：z=1 即近平面（Reversed-Z），恒通过 GreaterEqual 深度比较 */
	gl_Position = a_Position;

	vert2frag.TexCoord = a_Position.xy * 0.5f + 0.5f;
}


#type fragment
#version 410 core

#include "../DeferredShaders/GBufferCommon.glsl"
#include "DebugViewCommon.glsl"

struct SVextex2Frag
{
	vec2 TexCoord;
};

in SVextex2Frag vert2frag;
layout(location = 0) out vec4 OutFragColor;

/* Overdraw 计数纹理与阴影累加纹理：只在对应档位有效，其余档位由 CPU 补黑
 * （声明的采样器每次绘制都必须有绑定） */
uniform sampler2D u_OverdrawTexture;
uniform sampler2D u_ShadowAccumTexture;

/* 调试档位：本 shader 处理 Surface 档（G-Buffer 直读）、Shadow 档（多光源平均可见度）与 Overdraw 热力图 */
uniform int u_DebugMode = 0;

/* 1 = 没有几何的像素原样保留（前向的天空已经画在底下），0 = 整屏覆写（延迟由天空 Pass 收背景） */
uniform int u_PreserveBackground = 0;

/* Overdraw 热力色：0（背景）近黑 → 绿 → 红（高重叠） */
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
	vec3 debug_color;
	if (u_DebugMode == kDebugShadow)
	{
		/* 逐光源累加的可见度乘权重与权重相除得平均可见度，没有灯够得着时读作全受光 */
		const vec4 accum = texture(u_ShadowAccumTexture, vert2frag.TexCoord);
		if (u_PreserveBackground != 0 && accum.b < 1.0f)
			discard;
		debug_color = vec3(accum.g > 1e-5f ? accum.r / accum.g : 1.0f);
	}
	else if (u_DebugMode == kDebugOverdraw)
	{
		debug_color = OverdrawHeat(texture(u_OverdrawTexture, vert2frag.TexCoord).r);
	}
	else
	{
		SGBufferData gbuffer;
		CalculateGBuffer(gbuffer, vert2frag.TexCoord);

		SMaterialInput mtl = MakeDebugMaterialInput(gbuffer.Albedo, gbuffer.WorldNormal,
			gbuffer.Roughness, gbuffer.Metallic);
		mtl.Specular = gbuffer.Specular;
		mtl.AO = gbuffer.AO;
		mtl.Emission = gbuffer.Emission;
		mtl.Ambient = gbuffer.Ambient;
		debug_color = DebugSurfaceChannel(u_DebugMode, mtl);
	}

	OutFragColor = vec4(debug_color, 1.0f);
}
