#type vertex
#version 410 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;

#include "../builtin/Uniforms.glsl"

struct SVertex2Frag
{
	vec3 WorldPosition;
	vec3 Normal;
	vec2 TexCoord;
};

layout (location = 0) out SVertex2Frag Output;

void main()
{
	vec4 world_pos = u_Local2WorldMat * vec4(a_Position, 1.0f);
	gl_Position = u_ViewProjectionMat * world_pos;

	Output.WorldPosition = world_pos.xyz;
	Output.Normal = mat3(u_Local2WorldMat) * a_Normal;
	Output.TexCoord = a_TexCoord;
}


#type fragment
#version 410 core

#include "../builtin/Uniforms.glsl"
#include "../builtin/Math.glsl"
#include "../builtin/BRDF.glsl"
#include "../builtin/IBL.glsl"

struct SVertex2Frag
{
	vec3 WorldPosition;
	vec3 Normal;
	vec2 TexCoord;
};

layout (location = 0) in SVertex2Frag Input;

layout (location = 0) out vec4 OutColor;

layout(binding = 0) uniform sampler2D u_AlbedoTexture;
layout(binding = 1) uniform sampler2D u_RoughnessTexture;
layout(binding = 2) uniform sampler2D u_MetalnessTexture;
layout(binding = 3) uniform sampler2D u_NormalTexture;

/* IBL 环境贴图（u_IrradianceMap / u_PrefilterMap / u_BRDFLut / u_UseIBL）来自
 * builtin/IBL.glsl —— 由渲染通道按最近探针 per-draw 绑定 */
layout(location = 0) uniform float u_EnvironmentStrength;

vec3 GetNormal()
{
	vec3 tangent_normal = texture(u_NormalTexture, Input.TexCoord).xyz * 2.0f - 1.0f;
	tangent_normal = vec3(tangent_normal.x, -tangent_normal.y, tangent_normal.z);
	return normalize(Input.Normal);
}

void main()
{
	vec3 N = GetNormal();
	vec3 V = normalize(u_ViewPos - Input.WorldPosition);

	vec3 albedo = pow(texture(u_AlbedoTexture, Input.TexCoord).rgb, vec3(2.2f));
	float metallic = texture(u_MetalnessTexture, Input.TexCoord).r;
	float roughness = clamp(texture(u_RoughnessTexture, Input.TexCoord).r, 0.04f, 1.0f);

	vec3 F0 = mix(vec3(0.04f), albedo, metallic);

	// 直接光照（单方向光），复用BRDF.glsl
	vec3 L = normalize(-u_LightDir);
	vec3 direct = BRDF(L, V, N, metallic, roughness, albedo);

	// 间接光（IBL）：u_UseIBL = 0（无探针 / 探针未烘焙 / 探针捕获自身）时不参与
	vec3 ambient = (u_UseIBL != 0)
		? EvaluateIBL(N, V, roughness, metallic, albedo) * u_EnvironmentStrength
		: vec3(0.0f);

	// 方向光环境项：direct 已经包含 (diff+spec)*NdotL
	vec3 color = ambient + u_ColorIntensity.rgb * u_ColorIntensity.a * direct;

	// 简单的ToneMapping + Gamma
	color = color / (color + vec3(1.0f));
	color = pow(color, vec3(1.0f / 2.2f));

	OutColor = vec4(color, 1.0f);
}
