#type vertex
#version 410 core

/* 顶点属性按引擎统一契约：UV 在槽位 3、切线在槽位 4（2 是颜色槽，别读错） */
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 3) in vec3 a_TexCoord;
layout(location = 4) in vec3 a_Tangent;

#include "../Builtin/Uniforms.glsl"

struct SVertex2Frag
{
	vec3 WorldPosition;
	vec3 Normal;
	vec2 TexCoord;
	vec3 Tangent;
};

layout (location = 0) out SVertex2Frag Output;

void main()
{
	vec4 world_pos = u_Local2WorldMat * vec4(a_Position, 1.0f);
	gl_Position = u_ViewProjectionMat * world_pos;

	Output.WorldPosition = world_pos.xyz;
	Output.Normal = mat3(u_Local2WorldMat) * a_Normal;
	Output.TexCoord = a_TexCoord.xy;
	Output.Tangent = mat3(u_Local2WorldMat) * a_Tangent;
}


#type fragment
#version 410 core

#include "../Builtin/Uniforms.glsl"
#include "../Builtin/Math.glsl"
#include "../Builtin/BRDF.glsl"
#include "IBLCommon.glsl"

struct SVertex2Frag
{
	vec3 WorldPosition;
	vec3 Normal;
	vec2 TexCoord;
	vec3 Tangent;
};

layout (location = 0) in SVertex2Frag Input;

layout (location = 0) out vec4 OutFragColor;

layout(binding = 0) uniform sampler2D u_AlbedoTexture;
layout(binding = 1) uniform sampler2D u_RoughnessTexture;
layout(binding = 2) uniform sampler2D u_MetalnessTexture;
layout(binding = 3) uniform sampler2D u_NormalTexture;

/* IBL 环境贴图 / u_UseIBL 来自同目录 IBLCommon.glsl，由渲染通道按最近探针 per-draw 绑定 */
layout(location = 0) uniform float u_EnvironmentStrength;

/* 法线贴图采样，TBN 由插值切线构建 */
vec3 GetNormal()
{
	vec3 T = normalize(Input.Tangent);
	vec3 N = normalize(Input.Normal);
	T = normalize(T - dot(T, N) * N);
	mat3 TBN = mat3(T, cross(T, N), N);

	vec3 tangent_normal = texture(u_NormalTexture, Input.TexCoord).xyz * 2.0f - 1.0f;
	return normalize(TBN * tangent_normal);
}

void main()
{
	vec3 N = GetNormal();
	vec3 V = normalize(u_ViewPos - Input.WorldPosition);

	vec3 albedo = SrgbToLinear(texture(u_AlbedoTexture, Input.TexCoord).rgb);
	float metallic = texture(u_MetalnessTexture, Input.TexCoord).r;
	float roughness = clamp(texture(u_RoughnessTexture, Input.TexCoord).r, 0.04f, 1.0f);

	/* 直接光照（单方向光） */
	vec3 L = normalize(-u_LightDir);
	vec3 direct = BRDF(L, V, N, metallic, roughness, albedo);

	/* 间接光：u_UseIBL = 0（无探针 / 探针未烘焙）时不参与 */
	vec3 ambient = (u_UseIBL != 0)
		? EvaluateIBL(N, V, roughness, metallic, albedo) * u_EnvironmentStrength
		: vec3(0.0f);

	/* 合成，再做简单 ToneMapping 和 Gamma */
	vec3 color = ambient + u_ColorIntensity.rgb * u_ColorIntensity.a * direct;
	color = color / (color + vec3(1.0f));
	color = pow(color, vec3(1.0f / 2.2f));

	OutFragColor = vec4(color, 1.0f);
}
