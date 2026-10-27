#ifndef GBUFFER_COMMON_GLSL
#define GBUFFER_COMMON_GLSL

struct SGBufferData
{
	vec3 Albedo;
	float Roughness;
	vec3 WorldNormal;
	float Metallic;
	vec3 Specular;
	float AO;
	vec3 Emission;
	float IOR;
	vec3 Ambient;
	float Anisotropy;
	vec3 WorldPosition;
	/* 接受阴影（0/1）：来自 GBuffer.glsl 写入的 GBufferTexture5.a */
	float ReceiveShadow;
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
	gbuffer.Roughness = color0.a;
	/* 法线解码：GBuffer 以 n * 0.5 + 0.5 编码（RGBA8 无符号格式存不了负值） */
	gbuffer.WorldNormal = color1.rgb * 2.0f - 1.0f;
	gbuffer.Metallic = color1.a;
	gbuffer.Specular = color2.rgb;
	gbuffer.AO = color2.a;
	gbuffer.Emission = color3.rgb;
	gbuffer.IOR = color3.a;
	gbuffer.Ambient = color4.rgb;
	gbuffer.Anisotropy = color4.a;
	gbuffer.WorldPosition = color5.rgb;
	gbuffer.ReceiveShadow = color5.a;
}

#endif // GBUFFER_COMMON_GLSL
