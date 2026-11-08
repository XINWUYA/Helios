#type vertex
#version 410 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec4 a_Color;
layout(location = 3) in vec3 a_TexCoord;
layout(location = 4) in vec3 a_Tangent;

/* 实例数据：每实例一条（世界矩阵四列 + 参数，x = ObjectId、y = 接受阴影） */
layout(location = 5) in vec4 a_InstanceMatrix0;
layout(location = 6) in vec4 a_InstanceMatrix1;
layout(location = 7) in vec4 a_InstanceMatrix2;
layout(location = 8) in vec4 a_InstanceMatrix3;
layout(location = 9) in vec4 a_InstanceParams;
/* 上一帧的世界矩阵：只给运动矢量用 */
layout(location = 10) in vec4 a_InstancePrevMatrix0;
layout(location = 11) in vec4 a_InstancePrevMatrix1;
layout(location = 12) in vec4 a_InstancePrevMatrix2;
layout(location = 13) in vec4 a_InstancePrevMatrix3;

#include "../Builtin/Uniforms.glsl"

struct SVextex2Frag
{
	vec3 Position;
	vec3 Normal;
	vec4 BaseColor;
	vec2 TexCoord;
#if MTL_NORMAL || MTL_PARALLAX
	vec3 Tangent;
#endif
	float ObjectId;
	float ReceiveShadow;
	/* 当前帧与上一帧的裁剪空间位置：片元里相除才算得出屏幕位移 */
	vec4 CurClip;
	vec4 PrevClip;
};

out SVextex2Frag vert2frag;

void main()
{
	/* 物体世界矩阵来自实例数据：同材质同网格的一批对象共用一次绘制 */
	mat4 local2world = mat4(a_InstanceMatrix0, a_InstanceMatrix1, a_InstanceMatrix2, a_InstanceMatrix3);
	mat4 prev_local2world = mat4(a_InstancePrevMatrix0, a_InstancePrevMatrix1,
		a_InstancePrevMatrix2, a_InstancePrevMatrix3);

	const vec4 local_position = vec4(a_Position, 1.0f);
	/* 光栅化用带抖动的矩阵；两帧的裁剪坐标用不带抖动的，位移里才只有真实运动 */
	vert2frag.CurClip = u_VelocityViewProjectionMat * (local2world * local_position);
	vert2frag.PrevClip = u_PrevVelocityViewProjectionMat * (prev_local2world * local_position);

	gl_Position = u_ViewProjectionMat * (local2world * local_position);

	vert2frag.Position = vec3(local2world * local_position);
	vert2frag.BaseColor = a_Color;
	vert2frag.TexCoord = a_TexCoord.xy;
	vert2frag.ObjectId = a_InstanceParams.x;
	vert2frag.ReceiveShadow = a_InstanceParams.y;

	/* 法线矩阵：非等比缩放下法线 / 切线要按逆转置矩阵变换。
	 * 切线只有法线图 / 视差档才用得上，关掉就不算也不传 */
	mat3 normalMat = transpose(inverse(mat3(local2world)));
	vert2frag.Normal = normalize(normalMat * a_Normal);
#if MTL_NORMAL || MTL_PARALLAX
	vert2frag.Tangent = normalize(normalMat * a_Tangent);
#endif
}


#type fragment
#version 410 core

#include "../Builtin/Math.glsl"
#include "../Builtin/Uniforms.glsl"
#include "../Builtin/MaterialInput.glsl"
#include "../Builtin/MaterialLit.glsl"
#include "../Builtin/Velocity.glsl"

layout(location = 0) out vec4 GBufferTexture0; /* rgb: Albedo; a: Roughness */
layout(location = 1) out vec4 GBufferTexture1; /* rgb: WorldNormal; a: Metallic */
layout(location = 2) out vec4 GBufferTexture2; /* rgb: Specular; a: AO */
layout(location = 3) out vec4 GBufferTexture3; /* rgb: Emission; a: IOR */
layout(location = 4) out vec4 GBufferTexture4; /* rgb: Ambient; a: Anisotropy */
layout(location = 5) out vec4 GBufferTexture5; /* rgb: WorldPosition; a: 接受阴影(0/1) */
layout(location = 6) out float ObjectId;
layout(location = 7) out vec2 Velocity;        /* 屏幕空间运动矢量（UV 单位） */

struct SVextex2Frag
{
	vec3 Position;
	vec3 Normal;
	vec4 BaseColor;
	vec2 TexCoord;
#if MTL_NORMAL || MTL_PARALLAX
	vec3 Tangent;
#endif
	float ObjectId;
	float ReceiveShadow;
	vec4 CurClip;
	vec4 PrevClip;
};

in SVextex2Frag vert2frag;

void main()
{
	/* 材质采样走公共库（与前向材质着色器同一份，见 Builtin/MaterialLit.glsl） */
	SMaterialInput mtl;
	/* 变体关掉切线时传零向量，MaterialLit 只在 TBN 分支里读它 */
#if MTL_NORMAL || MTL_PARALLAX
	const vec3 world_tangent = vert2frag.Tangent;
#else
	const vec3 world_tangent = vec3(0.0f);
#endif
	CalculateLitMaterial(mtl, vert2frag.Position, vert2frag.TexCoord,
		vert2frag.BaseColor, vert2frag.Normal, world_tangent);

	GBufferTexture0 = vec4(mtl.Albedo, mtl.Roughness);
	/* 法线按 n*0.5+0.5 编码：RGBA8 存不了负值（解码在 GBufferCommon.glsl） */
	GBufferTexture1 = vec4(mtl.Normal * 0.5f + 0.5f, mtl.Metallic);
	GBufferTexture2 = vec4(mtl.Specular, mtl.AO);
	GBufferTexture3 = vec4(mtl.Emission, mtl.IOR);
	GBufferTexture4 = vec4(mtl.Ambient, mtl.Anisotropy);
	/* a 通道 = 接受阴影标记：延迟光照阶段没有逐对象数据，只能随 G-Buffer 带过去 */
	GBufferTexture5 = vec4(vert2frag.Position, vert2frag.ReceiveShadow);

	ObjectId = vert2frag.ObjectId;
	Velocity = ComputeVelocityFromClip(vert2frag.CurClip, vert2frag.PrevClip);
}
