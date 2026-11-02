/* 草叶调试重绘的顶点阶段：叶片展开与块底面跟可见着色、阴影投影共用同一套数学
 * （Builtin/GrassBlade.glsl），形状、风、脚印都对得上。DebugGrass / DebugGrassShadow 共用这一份。 */

layout(location = 0) in vec3 a_Position;

layout(location = 5) in vec4 a_InstanceMatrix0;
layout(location = 6) in vec4 a_InstanceMatrix1;
layout(location = 7) in vec4 a_InstanceMatrix2;
layout(location = 8) in vec4 a_InstanceMatrix3;
layout(location = 9) in vec4 a_InstanceParams;

/* 1 = 块底面，0 = 叶片（与 MaterialShaders/Grass.glsl 同一个开关） */
uniform int u_FlatPad = 0;                        // @property
uniform float u_PadCullDistance = 160.0f;         // @property

#include "../Builtin/Uniforms.glsl"
#include "../Builtin/GrassBlade.glsl"

struct SVextex2Frag
{
	vec3 WorldPosition;
	vec3 WorldNormal;
	float Height;
	float ColorSeed;
	float ReceiveShadow;
};

out SVextex2Frag vert2frag;

void main()
{
	const vec3 base = a_InstanceMatrix3.xyz;
	const vec3 right = a_InstanceMatrix0.xyz;
	const vec3 up = a_InstanceMatrix1.xyz;
	const vec3 face = a_InstanceMatrix2.xyz;

	vert2frag.ColorSeed = a_InstanceParams.w;
	vert2frag.ReceiveShadow = a_InstanceParams.y;

	/* 块底面：单位四边形铺在叶根高度，不吃风也不吃压弯 */
	if (u_FlatPad != 0)
	{
		/* 太远的块整块退化掉，与可见着色同口径 */
		if (length(u_ViewPos - base) > u_PadCullDistance)
		{
			gl_Position = vec4(0.0f, 0.0f, -2.0f, 1.0f);
			return;
		}

		const vec3 flat_pos = base + right * a_Position.x + face * a_Position.z;
		gl_Position = u_ViewProjectionMat * vec4(flat_pos, 1.0f);
		vert2frag.WorldPosition = flat_pos;
		vert2frag.WorldNormal = vec3(0.0f, 1.0f, 0.0f);
		vert2frag.Height = 0.0f;
		return;
	}

	/* 抽稀与可见着色同一套，调试视图里看到的草才是同一批 */
	if (GrassBladeFaded(base, a_InstanceParams))
	{
		gl_Position = vec4(0.0f, 0.0f, -2.0f, 1.0f);
		return;
	}

	vec3 blade_normal;
	const vec3 pos = ExpandGrassBlade(a_Position, base, right, up, face, a_InstanceParams,
		u_WindTime, blade_normal);
	gl_Position = u_ViewProjectionMat * vec4(pos, 1.0f);
	vert2frag.WorldPosition = pos;
	vert2frag.WorldNormal = blade_normal;
	vert2frag.Height = a_Position.y;
}
