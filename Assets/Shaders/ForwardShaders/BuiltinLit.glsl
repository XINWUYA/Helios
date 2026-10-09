#type vertex
#version 410 core

/* 顶点属性按引擎统一契约（见 Model.cpp 的 SetBuiltinVertexLayout / 导入 .mesh 的元素声明序）：
 * location 0..4 = 位置 / 法线 / 颜色 / UV / 切线 —— 本 shader 只用到 0/1/3，
 * 但 UV 的槽位是 3（不是 2；2 是颜色槽，读错槽位会拿到顶点色当 UV 使）。 */
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 3) in vec3 a_TexCoord;

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
	vert2frag.TexCoord      = a_TexCoord.xy;

	/* 法线矩阵：处理非等比缩放 */
	mat3 normal_mat = transpose(inverse(mat3(u_Local2WorldMat)));
	vert2frag.WorldNormal = normalize(normal_mat * a_Normal);
}


#type fragment
#version 410 core

#include "../builtin/Math.glsl"
#include "../builtin/Uniforms.glsl"
#include "../builtin/BRDF.glsl"
#include "../builtin/IBL.glsl"
#include "../builtin/ShadowUtils.glsl"
#include "../builtin/DirectLight.glsl"

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

	/* Albedo 转到线性空间后再做光照（应用主贴图 UV 缩放与偏移） */
	vec2 albedo_uv = vert2frag.TexCoord * u_AlbedoTilingOffset.xy + u_AlbedoTilingOffset.zw;
	vec3 albedo = SrgbToLinear(texture(u_AlbedoTexture, albedo_uv).rgb);

	/* 简单光照模型：金属度/粗糙度使用默认值 */
	float metallic  = 0.0f;
	float roughness = 0.8f;

	/* 直接光：按本笔光源的类型（方向光 / 点光 / 聚光）求光向、衰减与阴影 */
	vec3 L;
	float attenuation;
	float shadow;
	EvaluateDirectLight(u_ShadowMap, vert2frag.WorldPosition, N, L, attenuation, shadow);
	/* 模型关闭「接受阴影」时阴影因子归零 */
	shadow *= u_ReceiveShadow;

	/* 直接光照 */
	vec3 direct = max(vec3(0.0f), BRDF(L, V, N, metallic, roughness, albedo));
	direct *= u_ColorIntensity.rgb * u_ColorIntensity.a * attenuation;
	direct *= (1.0f - shadow * 0.8f);

	/* 间接光：反射探针 IBL（u_UseIBL = 1 时逐绘制绑定环境贴图）；没有探针就退回简单环境项。
	 * 跟光源无关，只在合成的那一笔（第 0 笔）里累加 —— 其余笔只叠直接光。 */
	vec3 color = direct;
	if (u_ComposeAmbientEmission > 0)
	{
		vec3 ambient;
		if (u_UseIBL != 0)
			ambient = max(vec3(0.0f), EvaluateIBL(N, V, roughness, metallic, albedo));
		else
			ambient = albedo * 0.3f;

		color += ambient;
	}

	OutFragColor = vec4(color, 1.0f);
	ObjectId = u_ObjectId;
}
