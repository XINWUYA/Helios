#type vertex
#version 410 core

/* 顶点声明与内建网格布局对齐（0..4 = 位置 / 法线 / 颜色 / UV / 切线）：
 * 本 shader 只读位置，声明保持一致以免属性槽类型错位 */
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec4 a_Color;
layout(location = 3) in vec3 a_TexCoord;
layout(location = 4) in vec3 a_Tangent;

#include "../Builtin/Uniforms.glsl"

struct SVextex2Frag
{
	vec3 Position;
};

layout (location = 0) out SVextex2Frag Output;

void main()
{
	mat4 view_mat = mat4(mat3(u_ViewMat));
	vec4 position = u_ProjectionMat * view_mat * vec4(a_Position, 1.0f);
	/* Reversed-Z：远平面深度为 0，z 置 0 让天空盒始终在最远处 */
	gl_Position = vec4(position.xy, 0.0f, position.w);

	Output.Position = a_Position;
}


#type fragment
#version 410 core

layout(location = 0) out vec4 OutFragColor;

struct SVextex2Frag
{
	vec3 Position;
};

layout (location = 0) in SVextex2Frag Input;

uniform sampler2D u_SkyTex;

/* 方向 → 等距柱状贴图 UV：与 ReflectionProbe/EquirectToCube.glsl 是同一映射
 * （改一处要同步另一处，否则天空盒与探针烘焙的环境对不上） */
const vec2 invAtan = vec2(0.1591, 0.3183);
vec2 SampleSphericalMap(vec3 v)
{
	vec2 uv = vec2(atan(v.z, v.x), asin(v.y));
	uv *= invAtan;
	uv += 0.5;
	return uv;
}

void main()
{
	vec2 uv = SampleSphericalMap(Input.Position);
	vec4 texture_color = texture(u_SkyTex, uv);
	OutFragColor = vec4(texture_color.rgb, 1.0f);
}
