#type vertex
#version 410 core

/* 顶点属性按引擎统一契约（location 0..4 = 位置 / 法线 / 颜色 / UV / 切线），这里只用 0/2/3 */
layout(location = 0) in vec3 a_Position;
layout(location = 2) in vec4 a_Color;
layout(location = 3) in vec3 a_TexCoord;

#include "../Builtin/Uniforms.glsl"

struct SVextex2Frag
{
	vec4 BaseColor;
	vec2 TexCoord;
};

out SVextex2Frag vert2frag;

void main()
{
	gl_Position = u_ViewProjectionMat * u_Local2WorldMat * vec4(a_Position, 1.0f);
	vert2frag.BaseColor = a_Color;
	vert2frag.TexCoord = a_TexCoord.xy;
}


#type fragment
#version 410 core

/* 无光照材质：不声明 u_ComposeAmbientEmission（前向按单笔原样提交），
 * 延迟管线下转成 GBuffer.glsl 的无光照变体写进 G-Buffer */
#include "../Builtin/Uniforms.glsl"
#include "../Builtin/MaterialUnlit.glsl"

/* 不透明度乘数：透明队列的材质靠它整体调淡；不透明队列不混合，改了看不见 */
uniform float u_Opacity = 1.0f; // @group(Surface) @property @range(0, 1)

struct SVextex2Frag
{
	vec4 BaseColor;
	vec2 TexCoord;
};

in SVextex2Frag vert2frag;

layout(location = 0) out vec4 OutFragColor;
layout(location = 1) out float ObjectId;

void main()
{
	const vec4 color = CalculateUnlitColor(vert2frag.TexCoord, vert2frag.BaseColor);
	OutFragColor = vec4(color.rgb, color.a * u_Opacity);
	ObjectId = float(u_ObjectId);
}
