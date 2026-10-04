#type vertex
#version 410 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;

#include "builtin/Uniforms.glsl"

struct SVextex2Frag
{
	vec3 Position;
};

layout (location = 0) out SVextex2Frag Output;

void main()
{
	mat4 view_mat = mat4(mat3(u_ViewMat));
	vec4 position = u_ProjectionMat * view_mat * vec4(a_Position, 1.0f);
	/* Reversed-Z：远平面深度为 0，将 z 置 0 使天空盒始终位于最远处 */
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