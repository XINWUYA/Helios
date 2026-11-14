#ifndef GBUFFER_COMMON_GLSL
#define GBUFFER_COMMON_GLSL

#include "ShadingModel.glsl"
#include "../Builtin/Uniforms.glsl"

/* 通道分组按访问模式排：基色与 AO 一起读、法线与接受阴影同属几何、
 * 三个 BRDF 标量挤一张。改布局要同步 GBuffer.glsl 与 Terrain.glsl 的写出。 */
struct SGBufferData
{
	vec3 Albedo;          /* Texture0.rgb */
	float AO;             /* Texture0.a */
	vec3 WorldNormal;     /* Texture1.rgb，n * 0.5 + 0.5 */
	float ReceiveShadow;  /* Texture1.a，2 bit 只存 0/1 */
	float Roughness;      /* Texture2.r */
	float Metallic;       /* Texture2.g */
	float Specular;       /* Texture2.b，标量，材质里三通道通常一样 */
	int ShadingModel;     /* Texture2.a，编号见 ShadingModel.glsl */
	vec4 CustomData;      /* Texture3，语义按 ShadingModel 解释，取值处各自说明 */
	vec3 Ambient;         /* Texture4.rgb */
	float Anisotropy;     /* Texture4.a */
	vec3 WorldPosition;   /* 由 u_GBufferDepth 反投影重建，不占 G-Buffer 通道 */
};

uniform sampler2D u_GBufferTexture0;
uniform sampler2D u_GBufferTexture1;
uniform sampler2D u_GBufferTexture2;
uniform sampler2D u_GBufferTexture3;
uniform sampler2D u_GBufferTexture4;
/* 深度附件：世界位置改成从它反投影，G-Buffer 不再存位置 */
uniform sampler2D u_GBufferDepth;

/* G-Buffer 一律按整数坐标逐像素读：texture() 要靠 2x2 邻居算导数，
 * 调用方提前 return 后导数失效，深度边界会混进背景像素 */
ivec2 GBufferTexel(vec2 uv)
{
	return ivec2(uv * vec2(textureSize(u_GBufferDepth, 0)));
}

float LoadGBufferDepth(vec2 uv)
{
	return texelFetch(u_GBufferDepth, GBufferTexel(uv), 0).r;
}

/* 深度反投影：uv 与深度还原世界坐标。深度是窗口空间的 [0,1]，换回 NDC 要乘 2 减 1 */
vec3 ReconstructWorldPosition(vec2 uv)
{
	const float depth = LoadGBufferDepth(uv);
	/* 深度已经是 D3D 风格的 [0,1]，直接当 NDC z 用；uv 的 (0,0) 与 NDC 左下角一致 */
	const vec4 ndc = vec4(uv * 2.0f - 1.0f, depth, 1.0f);
	const vec4 world = u_InvViewProjectionMat * ndc;
	return world.xyz / world.w;
}

void CalculateGBuffer(inout SGBufferData gbuffer, vec2 uv)
{
	const ivec2 texel = GBufferTexel(uv);
	vec4 color0 = texelFetch(u_GBufferTexture0, texel, 0);
	vec4 color1 = texelFetch(u_GBufferTexture1, texel, 0);
	vec4 color2 = texelFetch(u_GBufferTexture2, texel, 0);
	vec4 color3 = texelFetch(u_GBufferTexture3, texel, 0);
	vec4 color4 = texelFetch(u_GBufferTexture4, texel, 0);

	gbuffer.Albedo = color0.rgb;
	gbuffer.AO = color0.a;
	/* 法线解码：10:10:10 是无符号格式，写进去的是 n * 0.5 + 0.5 */
	gbuffer.WorldNormal = color1.rgb * 2.0f - 1.0f;
	gbuffer.ReceiveShadow = color1.a;
	gbuffer.Roughness = color2.r;
	gbuffer.Metallic = color2.g;
	gbuffer.Specular = color2.b;
	gbuffer.ShadingModel = DecodeShadingModel(color2.a);
	/* 第三张是自定义数据，语义由 ShadingModel 决定，用到的两处各自解释 */
	gbuffer.CustomData = color3;
	gbuffer.Ambient = color4.rgb;
	gbuffer.Anisotropy = color4.a;
	/* 反投影版：位置还在验证，阴影/环境光可能整体偏移；对不上就改回 color5.rgb */
	gbuffer.WorldPosition = ReconstructWorldPosition(uv);
}

#endif // GBUFFER_COMMON_GLSL
