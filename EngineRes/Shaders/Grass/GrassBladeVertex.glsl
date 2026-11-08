/* 草叶顶点阶段：叶片展开、块底面退化、远处抽稀。可见着色 / 调试重绘 / 调试阴影共用这一份，
 * 形状、风、抽稀对得上，调试看到的才是同一批草。 */

layout(location = 0) in vec3 a_Position;

/* 实例数据：每株一条。前三列已带缩放（x = 叶宽方向、y = 叶高、z = 叶面朝向），
 * 第四列是根部世界坐标；参数 x 保留给 ObjectId、y 接受阴影、z 相位、w 色彩变化 */
layout(location = 5) in vec4 a_InstanceMatrix0;
layout(location = 6) in vec4 a_InstanceMatrix1;
layout(location = 7) in vec4 a_InstanceMatrix2;
layout(location = 8) in vec4 a_InstanceMatrix3;
layout(location = 9) in vec4 a_InstanceParams;

#include "../Builtin/Uniforms.glsl"
#include "GrassBlade.glsl"

/* 1 = 画的是块底面（叶根下的草色垫层），0 = 叶片 */
uniform int u_FlatPad = 0;                        // @group(Ground pad) @property
/* 底面比叶片画得更远：叶片停画之后，草色还能再撑一段 */
uniform float u_PadCullDistance = 160.0f;         // @group(Ground pad) @property

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

	/* 块底面：单位四边形直接铺在叶根高度，不吃风也不吃压弯 */
	if (u_FlatPad != 0)
	{
		/* 太远的块整块退化掉：远景不该留一块漂浮的绿板 */
		if (length(u_ViewPos - base) > u_PadCullDistance)
		{
			gl_Position = vec4(0.0f, 0.0f, -2.0f, 1.0f);
			return;
		}

		const vec3 flat_pos = base + right * a_Position.x + face * a_Position.z;
		gl_Position = u_ViewProjectionMat * vec4(flat_pos, 1.0f);
		vert2frag.WorldNormal = vec3(0.0f, 1.0f, 0.0f);
		vert2frag.WorldPosition = flat_pos;
		vert2frag.Height = 0.0f;
		return;
	}

	/* 远处逐株抽稀：块内密度连续，块边界不会拉出一条竖边 */
	const float view_distance = length(u_ViewPos - base);
	if (GrassBladeFaded(view_distance, a_InstanceParams))
	{
		gl_Position = vec4(0.0f, 0.0f, -2.0f, 1.0f);
		return;
	}

	vec3 blade_normal;
	const vec3 pos = ExpandGrassBlade(a_Position, base, right, up, face, a_InstanceParams,
		GrassWindWeight(view_distance), u_WindPhase, blade_normal);
	gl_Position = u_ViewProjectionMat * vec4(pos, 1.0f);
	vert2frag.WorldPosition = pos;
	vert2frag.WorldNormal = blade_normal;
	vert2frag.Height = a_Position.y;
}
