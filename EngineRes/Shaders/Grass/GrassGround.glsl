#type vertex
#version 410 core

/* 地面：普通贴图地面 + 按距离接管草色。
 * 顶点属性按引擎统一契约（location 0/1/3 = 位置 / 法线 / UV）。 */
// @variant(DEBUG_VIEW, draw)
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 3) in vec3 a_TexCoord;

#include "../Builtin/Uniforms.glsl"

struct SVextex2Frag
{
	vec3 WorldPosition;
	vec3 WorldNormal;
	vec2 TexCoord;
};

out SVextex2Frag vert2frag;

void main()
{
	const vec4 world = u_Local2WorldMat * vec4(a_Position, 1.0f);
	gl_Position = u_ViewProjectionMat * world;

	vert2frag.WorldPosition = world.xyz;
	vert2frag.TexCoord = a_TexCoord.xy;

	const mat3 normal_mat = transpose(inverse(mat3(u_Local2WorldMat)));
	vert2frag.WorldNormal = normalize(normal_mat * a_Normal);
}


#type fragment
#version 410 core

#include "../Builtin/Math.glsl"
#include "../Builtin/Uniforms.glsl"
#include "../Builtin/MaterialInput.glsl"
#include "../Builtin/ForwardShade.glsl"

struct SVextex2Frag
{
	vec3 WorldPosition;
	vec3 WorldNormal;
	vec2 TexCoord;
};

in SVextex2Frag vert2frag;

layout(location = 0) out vec4 OutFragColor;
layout(location = 1) out float ObjectId;

uniform sampler2D u_GroundTexture;                                // @property
uniform vec4 u_GroundTiling = vec4(60.0f, 60.0f, 0.0f, 0.0f);     // @property
uniform vec3 u_GrassColor = vec3(0.22f, 0.34f, 0.10f);            // @property @type(color)
/* x = 开始接管的视距、y = 完全接管的视距、z = 边界向外柔化的米数、w = 开关 */
uniform vec4 u_GrassTakeover = vec4(18.0f, 70.0f, 12.0f, 1.0f);   // @property

/* 草地范围（xz 最小角与尺寸）：由渲染器逐视图填入 */
layout(std140, binding = 4) uniform GrassAreaBlock
{
	vec4 u_GrassAreaMin;
	vec4 u_GrassAreaSize;
};

void main()
{
	const vec2 ground_uv = vert2frag.TexCoord.xy * u_GroundTiling.xy + u_GroundTiling.zw;
	const vec4 ground_sample = texture(u_GroundTexture, ground_uv);
	vec3 albedo = SrgbToLinear(ground_sample.rgb);

	/* 覆盖判断：矩形内部为正、外部为负，边界外 softness 米之内淡出。
	 * 草地的块底面只铺到矩形边界，这里让它往外再延伸一段，接缝就看不出来了 */
	const vec2 world_xz = vert2frag.WorldPosition.xz;
	const vec2 local = world_xz - u_GrassAreaMin.xy;
	const vec2 edge = min(local, u_GrassAreaSize.xy - local);
	const float inside = min(edge.x, edge.y);
	const float area_mask = smoothstep(-u_GrassTakeover.z, 0.0f, inside);

	/* 距离接管：脚下还是地面自己的贴图（叶缝里看得到土），远处整片换成草色 */
	const float view_distance = length(u_ViewPos.xz - world_xz);
	const float distance_mask = smoothstep(u_GrassTakeover.x, u_GrassTakeover.y, view_distance);
	const float takeover = area_mask * distance_mask * u_GrassTakeover.w;

	/* 用世界坐标做一点疏密变化，免得整片纯色 */
	const float noise = 0.92f + 0.16f * fract(sin(dot(floor(world_xz * 0.5f), vec2(12.9898f, 78.233f))) * 43758.5453f);
	albedo = mix(albedo, u_GrassColor * noise, takeover);

	/* 地面用固定的粗糙度 / 金属度，没有切线也没有环境贴图 */
	SMaterialInput mtl = MakeMaterialInput(albedo, normalize(vert2frag.WorldNormal), 0.85f, 0.0f);

	ObjectId = float(u_ObjectId);

	const SForwardShade shade = ShadeForwardSurface(mtl, vert2frag.WorldPosition);
	OutFragColor = shade.DebugReadout ? shade.Color : vec4(shade.Color.rgb, 1.0f);
}
