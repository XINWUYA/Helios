#type vertex
#version 410 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec4 a_Color;
layout(location = 3) in vec3 a_TexCoord;
layout(location = 4) in vec3 a_Tangent;

#include "builtin/Uniforms.glsl"

struct SVextex2Frag
{
	vec3 Position;
	vec3 Normal;
	vec4 BaseColor;
	vec2 TexCoord;
	vec3 Tangent;
};

layout (location = 0) out SVextex2Frag vert2frag;

void main()
{
	gl_Position = u_ViewProjectionMat * u_Local2WorldMat * vec4(a_Position, 1.0f);

	vert2frag.Position = vec3(u_Local2WorldMat * vec4(a_Position, 1.0f));
	vert2frag.BaseColor = a_Color;
	vert2frag.TexCoord = a_TexCoord.xy;
	
	mat3 normalMat = transpose(inverse(mat3(u_Local2WorldMat)));
	vert2frag.Normal = normalize(normalMat *a_Normal);
	vert2frag.Tangent = normalize(normalMat * a_Tangent);
	/* Gram-Schmidt process:
	 * Re-orthogonalize T with respect to N
	 */
//	Output.Tangent = normalize(Output.Tangent - dot(Output.Tangent, Output.Normal) * Output.Normal);
}


#type fragment
#version 410 core

#include "common.glsl"
#include "builtin/Math.glsl"
#include "builtin/Uniforms.glsl"
#include "builtin/MaterialCommon.glsl"

layout(location = 0) out vec4 GBufferTexture0; /* rgb: Albedo; a: Roughness */
layout(location = 1) out vec4 GBufferTexture1; /* rgb: WorldNormal; a: Metallic */
layout(location = 2) out vec4 GBufferTexture2; /* rgb: Specular; a: AO */
layout(location = 3) out vec4 GBufferTexture3; /* rgb: Emission; a: IOR */
layout(location = 4) out vec4 GBufferTexture4; /* rgb: Ambient; a: Anisotropy */
layout(location = 5) out vec4 GBufferTexture5; /* rgb: WorldPosition; a: 接受阴影(0/1) */
layout(location = 6) out int  ObjectId;

struct SVextex2Frag
{
	vec3 Position;
	vec3 Normal;
	vec4 BaseColor;
	vec2 TexCoord;
	vec3 Tangent;
};

layout (location = 0) in SVextex2Frag vert2frag;

layout(binding = 0) uniform sampler2D u_AlbedoTexture;
layout(binding = 1) uniform sampler2D u_SpecularTexture;
layout(binding = 2) uniform sampler2D u_NormalTexture;
layout(binding = 3) uniform sampler2D u_BumpTexture;
layout(binding = 4) uniform sampler2D u_DisplacementTexture;
layout(binding = 5) uniform sampler2D u_RoughnessTexture;
layout(binding = 6) uniform sampler2D u_MetallicTexture;
layout(binding = 7) uniform sampler2D u_EmissiveTexture;
layout(binding = 8) uniform sampler2D u_AmbientTexture;

/* 主贴图 UV 缩放与偏移：xy = 缩放(tiling)，zw = 偏移(offset)。
 * 与 ForwardShaders（BuiltinLit / PBRStandard）同名同语义：普通材质在编辑器里
 * 转换为本 shader 渲染，该参数经此生效；缺省 vec4(1,1,0,0) 不变换。 */
layout(location = 0) uniform vec4 u_AlbedoTilingOffset = vec4(1.0f, 1.0f, 0.0f, 0.0f);

void CalculateMaterial(inout SMaterialInput mtl)
{
	/* 主贴图 UV 缩放与偏移（与 ForwardShaders 的采样口径一致） */
	vec2 base_uv = vert2frag.TexCoord * u_AlbedoTilingOffset.xy + u_AlbedoTilingOffset.zw;

	mtl.Albedo = texture(u_AlbedoTexture, base_uv).rgb;

	/* TBN */
	vec3 T = normalize(vert2frag.Tangent);
	vec3 N = normalize(vert2frag.Normal);
	vec3 B = cross(T, N);
	mat3 TBN = mat3(T, B, N);

	/* Calculate Parallax Mapping */
	vec3 view_dir = u_ViewPos - vert2frag.Position;
	view_dir = normalize(TBN * view_dir);
	float height = texture(u_BumpTexture, base_uv).x;
	vec2 uv = base_uv - view_dir.xy / view_dir.z * height;

	vec3 normal = texture(u_NormalTexture, uv).xyz * 2.0f - 1.0f;
	//normal.z = sqrt(max(1.0f - normal.x * normal.x - normal.y * normal.y, 0.0f));
	mtl.Normal = normalize(TBN * normal);
	
	mtl.Roughness = texture(u_RoughnessTexture, uv).r;
	mtl.Metallic = texture(u_MetallicTexture, uv).r;
	mtl.Specular = texture(u_SpecularTexture, uv).rgb;
	mtl.Emission = texture(u_EmissiveTexture, uv).rgb;
	mtl.Ambient = texture(u_AmbientTexture, uv).rgb;
	mtl.Anisotropy = 0.0f;
	mtl.AO = 1.0f;
	mtl.IOR = 0.0f;
	mtl.ClearCoatThickness = 0.0f;
	mtl.ClearCoatRoughness = 0.0f;
}

void main()
{
	
	SMaterialInput mtl;
	CalculateMaterial(mtl);
	
	GBufferTexture0 = vec4(mtl.Albedo, mtl.Roughness);
	GBufferTexture1 = vec4(mtl.Normal, mtl.Metallic);
	GBufferTexture2 = vec4(mtl.Specular, mtl.AO);
	GBufferTexture3 = vec4(mtl.Emission, mtl.IOR);
	GBufferTexture4 = vec4(mtl.Ambient, mtl.Anisotropy);
	/* a 通道承载"接受阴影"标记：延迟光照阶段没有逐对象数据，只能随 G-Buffer 带过去 */
	GBufferTexture5 = vec4(vert2frag.Position, u_ReceiveShadow);
	
	ObjectId = u_ObjectId;
}