#ifndef GBUFFER_COMMON_GLSL
#define GBUFFER_COMMON_GLSL

#include "ShadingModel.glsl"

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
	float ShadingModel;   /* Texture2.a，0 = 标准 PBR，1 = 双面植被 */
	vec4 CustomData;      /* Texture3，语义按 ShadingModel 解释，取值处各自说明 */
	vec3 Ambient;         /* Texture4.rgb */
	float Anisotropy;     /* Texture4.a */
	vec3 WorldPosition;   /* Texture5.rgb */
};

uniform sampler2D u_GBufferTexture0;
uniform sampler2D u_GBufferTexture1;
uniform sampler2D u_GBufferTexture2;
uniform sampler2D u_GBufferTexture3;
uniform sampler2D u_GBufferTexture4;
uniform sampler2D u_GBufferTexture5;

void CalculateGBuffer(inout SGBufferData gbuffer, vec2 uv)
{
	vec4 color0 = texture(u_GBufferTexture0, uv);
	vec4 color1 = texture(u_GBufferTexture1, uv);
	vec4 color2 = texture(u_GBufferTexture2, uv);
	vec4 color3 = texture(u_GBufferTexture3, uv);
	vec4 color4 = texture(u_GBufferTexture4, uv);
	vec4 color5 = texture(u_GBufferTexture5, uv);

	gbuffer.Albedo = color0.rgb;
	gbuffer.AO = color0.a;
	/* 法线解码：10:10:10 是无符号格式，写进去的是 n * 0.5 + 0.5 */
	gbuffer.WorldNormal = color1.rgb * 2.0f - 1.0f;
	gbuffer.ReceiveShadow = color1.a;
	gbuffer.Roughness = color2.r;
	gbuffer.Metallic = color2.g;
	gbuffer.Specular = color2.b;
	gbuffer.ShadingModel = color2.a;
	/* 第三张是自定义数据，语义由 ShadingModel 决定，用到的两处各自解释 */
	gbuffer.CustomData = color3;
	gbuffer.Ambient = color4.rgb;
	gbuffer.Anisotropy = color4.a;
	gbuffer.WorldPosition = color5.rgb;
}

#endif // GBUFFER_COMMON_GLSL
