#type vertex
#version 410 core

/* 调试读数整段只在调试变体里编进来，正常着色不带那份代码。
 * 视图打开调试档位时置 1，具体档位仍走 u_DebugView */
// @variant(DEBUG_VIEW, view)

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec4 a_Color;
layout(location = 3) in vec3 a_TexCoord;
layout(location = 4) in vec3 a_Tangent;

#include "../Builtin/Uniforms.glsl"

struct SVextex2Frag
{
	vec3 WorldPosition;
	vec3 WorldNormal;
	vec4 BaseColor;
	vec2 TexCoord;
#if MTL_NORMAL || MTL_PARALLAX
	vec3 WorldTangent;
#endif
};

out SVextex2Frag vert2frag;

void main()
{
	vec4 world_pos = u_Local2WorldMat * vec4(a_Position, 1.0f);

	gl_Position = u_ViewProjectionMat * world_pos;

	vert2frag.WorldPosition = world_pos.xyz;
	vert2frag.BaseColor     = a_Color;
	vert2frag.TexCoord      = a_TexCoord.xy;

	/* 法线矩阵：处理非等比缩放。切线只有法线图 / 视差档才用得上，关掉就不算也不传 */
	mat3 normal_mat = transpose(inverse(mat3(u_Local2WorldMat)));
	vert2frag.WorldNormal  = normalize(normal_mat * a_Normal);
#if MTL_NORMAL || MTL_PARALLAX
	vert2frag.WorldTangent = normalize(normal_mat * a_Tangent);
#endif
}


#type fragment
#version 410 core

#include "../Builtin/Math.glsl"
#include "../Builtin/Uniforms.glsl"
#include "../Builtin/MaterialInput.glsl"
#include "../Builtin/MaterialLit.glsl"
#include "../Builtin/ForwardShade.glsl"

/* 不透明度乘数：透明队列的材质靠它整体调淡；不透明队列不混合，改了看不见 */
uniform float u_Opacity = 1.0f; // @group(Surface) @property

struct SVextex2Frag
{
	vec3 WorldPosition;
	vec3 WorldNormal;
	vec4 BaseColor;
	vec2 TexCoord;
#if MTL_NORMAL || MTL_PARALLAX
	vec3 WorldTangent;
#endif
};

in SVextex2Frag vert2frag;

layout(location = 0) out vec4 OutFragColor;
layout(location = 1) out float ObjectId;

void main()
{
	SMaterialInput mtl;
	/* 变体关掉切线时传零向量，MaterialLit 只在 TBN 分支里读它 */
#if MTL_NORMAL || MTL_PARALLAX
	const vec3 world_tangent = vert2frag.WorldTangent;
#else
	const vec3 world_tangent = vec3(0.0f);
#endif
	CalculateLitMaterial(mtl, vert2frag.WorldPosition, vert2frag.TexCoord,
		vert2frag.BaseColor, vert2frag.WorldNormal, world_tangent);

	ObjectId = float(u_ObjectId);

	const SForwardShade shade = ShadeForwardSurface(mtl, vert2frag.WorldPosition);
	OutFragColor = shade.DebugReadout
		? shade.Color
		: vec4(shade.Color.rgb, mtl.Opacity * u_Opacity);
}
