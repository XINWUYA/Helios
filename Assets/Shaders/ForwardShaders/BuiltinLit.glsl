#type vertex
#version 410 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;

#include "../builtin/Uniforms.glsl"

struct SVextex2Frag
{
	vec3 WorldPosition;
	vec3 WorldNormal;
	vec2 TexCoord;
};

layout(location = 0) out SVextex2Frag vert2frag;

void main()
{
	vec4 world_pos = u_Local2WorldMat * vec4(a_Position, 1.0f);

	gl_Position = u_ViewProjectionMat * world_pos;

	vert2frag.WorldPosition = world_pos.xyz;
	vert2frag.TexCoord      = a_TexCoord;

	/* 法线矩阵：处理非等比缩放 */
	mat3 normal_mat = transpose(inverse(mat3(u_Local2WorldMat)));
	vert2frag.WorldNormal = normalize(normal_mat * a_Normal);
}


#type fragment
#version 410 core

#include "../builtin/Math.glsl"
#include "../builtin/Uniforms.glsl"
#include "../builtin/BRDF.glsl"
#include "../builtin/ShadowUtils.glsl"

struct SVextex2Frag
{
	vec3 WorldPosition;
	vec3 WorldNormal;
	vec2 TexCoord;
};

layout(location = 0) in SVextex2Frag vert2frag;

layout(location = 0) out vec4 OutFragColor;
layout(location = 1) out int  ObjectId;

/* 基础颜色贴图 */
layout(binding = 0) uniform sampler2D u_AlbedoTexture;

/* 主贴图 UV 缩放与偏移：xy = 缩放(tiling)，zw = 偏移(offset)
 * 注意：普通 uniform 的 layout(location) 必须与 sampler 的 layout(binding) 编号错开，
 * 否则在 SPIR-V 链接时会抢占同一编号导致采样错位。本 shader 的 sampler 占用 binding 0~9。 */
layout(location = 0) uniform vec4 u_AlbedoTilingOffset = vec4(1.0f, 1.0f, 0.0f, 0.0f);

/* 级联阴影 */
layout(binding = 9) uniform sampler2DArray u_ShadowMap;

/* -------------------------------------------------------------------------- */
/* 主函数                                                                      */
/* -------------------------------------------------------------------------- */
void main()
{
	/* 无切线数据，直接使用插值后的世界法线 */
	vec3 N = normalize(vert2frag.WorldNormal);
	vec3 V = normalize(u_ViewPos - vert2frag.WorldPosition);

	/* 方向光：u_LightDir 指向光源传播方向，取反得到指向光源的向量 */
	vec3 L = -normalize(u_LightDir);

	/* Albedo 转到线性空间后再做光照（应用主贴图 UV 缩放与偏移） */
	vec2 albedo_uv = vert2frag.TexCoord * u_AlbedoTilingOffset.xy + u_AlbedoTilingOffset.zw;
	vec3 albedo = SrgbToLinear(texture(u_AlbedoTexture, albedo_uv).rgb);

	/* 简单光照模型：金属度/粗糙度使用默认值 */
	float metallic  = 0.0f;
	float roughness = 0.8f;

	vec3 view_pos = (u_ViewMat * vec4(vert2frag.WorldPosition, 1.0f)).xyz;
	float shadow = CalculateShadow(u_ShadowMap, vert2frag.WorldPosition, view_pos, N, L);
	/* 模型关闭「接受阴影」时阴影因子归零 */
	shadow *= u_ReceiveShadow;

	/* 直接光照 */
	vec3 direct = max(vec3(0.0f), BRDF(L, V, N, metallic, roughness, albedo));
	direct *= u_ColorIntensity.rgb * u_ColorIntensity.a;
	direct *= (1.0f - shadow * 0.8f);

	/* 简单环境光 */
	vec3 ambient = albedo * 0.3f;

	vec3 color = direct + ambient;

	OutFragColor = vec4(color, 1.0f);
	ObjectId = u_ObjectId;
}
