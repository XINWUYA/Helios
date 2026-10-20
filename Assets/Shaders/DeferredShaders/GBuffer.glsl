#type vertex
#version 410 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec4 a_Color;
layout(location = 3) in vec3 a_TexCoord;
layout(location = 4) in vec3 a_Tangent;

#include "../Builtin/Uniforms.glsl"

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

	/* 法线矩阵：非等比缩放下法线 / 切线要按逆转置矩阵变换 */
	mat3 normalMat = transpose(inverse(mat3(u_Local2WorldMat)));
	vert2frag.Normal = normalize(normalMat * a_Normal);
	vert2frag.Tangent = normalize(normalMat * a_Tangent);
}


#type fragment
#version 410 core

#include "../Builtin/Math.glsl"
#include "../Builtin/Uniforms.glsl"
#include "../Builtin/MaterialInput.glsl"
#include "../Builtin/MaterialLit.glsl"

layout(location = 0) out vec4 GBufferTexture0; /* rgb: Albedo; a: Roughness */
layout(location = 1) out vec4 GBufferTexture1; /* rgb: WorldNormal; a: Metallic */
layout(location = 2) out vec4 GBufferTexture2; /* rgb: Specular; a: AO */
layout(location = 3) out vec4 GBufferTexture3; /* rgb: Emission; a: IOR */
layout(location = 4) out vec4 GBufferTexture4; /* rgb: Ambient; a: Anisotropy */
layout(location = 5) out vec4 GBufferTexture5; /* rgb: WorldPosition; a: 接受阴影(0/1) */
layout(location = 6) out float ObjectId;

struct SVextex2Frag
{
	vec3 Position;
	vec3 Normal;
	vec4 BaseColor;
	vec2 TexCoord;
	vec3 Tangent;
};

layout (location = 0) in SVextex2Frag vert2frag;

void main()
{
	/* 材质采样走公共库（与前向材质着色器同一份，见 Builtin/MaterialLit.glsl） */
	SMaterialInput mtl;
	CalculateLitMaterial(mtl, vert2frag.Position, vert2frag.TexCoord,
		vert2frag.BaseColor, vert2frag.Normal, vert2frag.Tangent);

	GBufferTexture0 = vec4(mtl.Albedo, mtl.Roughness);
	/* 法线按 n*0.5+0.5 编码：RGBA8 存不了负值（解码在 GBufferCommon.glsl） */
	GBufferTexture1 = vec4(mtl.Normal * 0.5f + 0.5f, mtl.Metallic);
	GBufferTexture2 = vec4(mtl.Specular, mtl.AO);
	GBufferTexture3 = vec4(mtl.Emission, mtl.IOR);
	GBufferTexture4 = vec4(mtl.Ambient, mtl.Anisotropy);
	/* a 通道 = 接受阴影标记：延迟光照阶段没有逐对象数据，只能随 G-Buffer 带过去 */
	GBufferTexture5 = vec4(vert2frag.Position, u_ReceiveShadow);

	ObjectId = float(u_ObjectId);
}
