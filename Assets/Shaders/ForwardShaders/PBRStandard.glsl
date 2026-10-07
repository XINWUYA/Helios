#type vertex
#version 410 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec4 a_Color;
layout(location = 3) in vec3 a_TexCoord;
layout(location = 4) in vec3 a_Tangent;

#include "../builtin/Uniforms.glsl"

struct SVextex2Frag
{
	vec3 WorldPosition;
	vec3 WorldNormal;
	vec4 BaseColor;
	vec2 TexCoord;
	vec3 WorldTangent;
};

layout(location = 0) out SVextex2Frag vert2frag;

void main()
{
	vec4 world_pos = u_Local2WorldMat * vec4(a_Position, 1.0f);

	gl_Position = u_ViewProjectionMat * world_pos;

	vert2frag.WorldPosition = world_pos.xyz;
	vert2frag.BaseColor     = a_Color;
	vert2frag.TexCoord      = a_TexCoord.xy;

	/* 法线矩阵：处理非等比缩放 */
	mat3 normal_mat = transpose(inverse(mat3(u_Local2WorldMat)));
	vert2frag.WorldNormal  = normalize(normal_mat * a_Normal);
	vert2frag.WorldTangent = normalize(normal_mat * a_Tangent);
}


#type fragment
#version 410 core

#include "../builtin/Math.glsl"
#include "../builtin/Uniforms.glsl"
#include "../builtin/BRDF.glsl"
#include "../builtin/MaterialCommon.glsl"
#include "../builtin/ShadowUtils.glsl"

struct SVextex2Frag
{
	vec3 WorldPosition;
	vec3 WorldNormal;
	vec4 BaseColor;
	vec2 TexCoord;
	vec3 WorldTangent;
};

layout(location = 0) in SVextex2Frag vert2frag;

layout(location = 0) out vec4 OutFragColor;
layout(location = 1) out int  ObjectId;

/* 材质贴图，绑定点与 default.glsl 保持一致 */
layout(binding = 0) uniform sampler2D u_AlbedoTexture;
layout(binding = 1) uniform sampler2D u_SpecularTexture;
layout(binding = 2) uniform sampler2D u_NormalTexture;
layout(binding = 3) uniform sampler2D u_BumpTexture;
layout(binding = 4) uniform sampler2D u_DisplacementTexture;
layout(binding = 5) uniform sampler2D u_RoughnessTexture;
layout(binding = 6) uniform sampler2D u_MetallicTexture;
layout(binding = 7) uniform sampler2D u_EmissiveTexture;
layout(binding = 8) uniform sampler2D u_AmbientTexture;
layout(binding = 9) uniform sampler2DArray u_ShadowMap; /* 级联阴影 */

/* 主贴图 UV 缩放与偏移：xy = 缩放(tiling)，zw = 偏移(offset)
 * 普通 uniform 的 layout(location) 需与上方 sampler 的 layout(binding) 0~9 错开。 */
layout(location = 0) uniform vec4 u_AlbedoTilingOffset = vec4(1.0f, 1.0f, 0.0f, 0.0f);

/* -------------------------------------------------------------------------- */
/* 材质采样                                                                    */
/* -------------------------------------------------------------------------- */
void CalculateMaterial(inout SMaterialInput mtl)
{
	/* 构建 TBN，用于法线贴图与视差映射 */
	vec3 T = normalize(vert2frag.WorldTangent);
	vec3 N = normalize(vert2frag.WorldNormal);
	/* Gram-Schmidt 正交化 */
	T = normalize(T - dot(T, N) * N);
	vec3 B = cross(T, N);
	mat3 TBN = mat3(T, B, N);

	/* 主贴图 UV 缩放与偏移（统一作用于所有材质贴图采样） */
	vec2 base_uv = vert2frag.TexCoord * u_AlbedoTilingOffset.xy + u_AlbedoTilingOffset.zw;

	/* 视差映射 */
	vec3 view_dir = normalize(TBN * (u_ViewPos - vert2frag.WorldPosition));
	float height = texture(u_BumpTexture, base_uv).x;
	vec2 uv = base_uv - view_dir.xy / max(view_dir.z, 1e-3f) * height;

	/* Albedo 转到线性空间后再做光照 */
	mtl.Albedo = SrgbToLinear(texture(u_AlbedoTexture, uv).rgb) * vert2frag.BaseColor.rgb;

	/* 法线贴图 */
	vec3 tangent_normal = texture(u_NormalTexture, uv).xyz * 2.0f - 1.0f;
	mtl.Normal = normalize(TBN * tangent_normal);

	mtl.Roughness   = texture(u_RoughnessTexture, uv).r;
	mtl.Metallic    = texture(u_MetallicTexture, uv).r;
	mtl.Specular    = texture(u_SpecularTexture, uv).rgb;
	mtl.Emission    = texture(u_EmissiveTexture, uv).rgb;
	mtl.Ambient     = texture(u_AmbientTexture, uv).rgb;
	mtl.AO          = 1.0f;
	mtl.Anisotropy  = 0.0f;
	mtl.IOR         = 0.0f;
	mtl.ClearCoatThickness = 0.0f;
	mtl.ClearCoatRoughness = 0.0f;
}

/* -------------------------------------------------------------------------- */
/* 主函数                                                                      */
/* -------------------------------------------------------------------------- */
void main()
{
	SMaterialInput mtl;
	CalculateMaterial(mtl);

	vec3 N = normalize(mtl.Normal);
	vec3 V = normalize(u_ViewPos - vert2frag.WorldPosition);

	/* 方向光：u_LightDir 指向光源传播方向，取反得到指向光源的向量 */
	vec3 L = -normalize(u_LightDir);

	vec3 view_pos = (u_ViewMat * vec4(vert2frag.WorldPosition, 1.0f)).xyz;
	float shadow = CalculateShadow(u_ShadowMap, vert2frag.WorldPosition, view_pos, N, L);
	/* 模型关闭「接受阴影」时阴影因子归零 */
	shadow *= u_ReceiveShadow;

	/* 直接光照 */
	vec3 direct = max(vec3(0.0f), BRDF(L, V, N, mtl.Metallic, mtl.Roughness, mtl.Albedo));
	direct *= u_ColorIntensity.rgb * u_ColorIntensity.a;
	direct *= (1.0f - shadow * 0.8f);

	/* 环境光 + 自发光 */
	vec3 ambient  = max(vec3(0.0f), mtl.Ambient * mtl.Albedo * mtl.AO * 0.3f);
	vec3 emission = mtl.Emission;

	vec3 color = direct + ambient + emission;

	OutFragColor = vec4(color, 1.0f);
	ObjectId = u_ObjectId;
}
