#type vertex
#version 410 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Color;
layout(location = 2) in vec3 a_Dir;		/* 线方向（单位向量）：网格填所在轴；轴带 = 轴向 */
layout(location = 3) in float a_Side;	/* 轴带两侧：-1 / +1；网格与普通线填 0 */

#include "builtin/Uniforms.glsl"

layout (location = 0) out vec3 v_Color;
layout (location = 1) out vec3 v_WorldPos;
layout (location = 2) noperspective out float v_Side;
layout (location = 3) out vec3 v_Dir;

/* 轴带宽度与视口尺寸（均为物理像素），由材质参数写入 —— 见 EditorBuiltinCamera */
uniform float u_LineWidth;
uniform vec2 u_ViewportSize;

void main()
{
	/* 场景 gizmo（地面网格 / 坐标轴）都在世界空间：网格用模型矩阵随相机
	 * 平移（按格对齐，线仍落在世界整数坐标上），坐标轴用单位矩阵 */
	const vec4 world_pos = u_Local2WorldMat * vec4(a_Position, 1.0f);
	vec4 clip_pos = u_ViewProjectionMat * world_pos;

	/* 坐标轴是"屏幕空间等宽的带"：几何退化成零宽线，这里按像素向两侧展开（Line 图元在 Metal 上
	 * 加不了宽、也没有抗锯齿）。展开方向 = 轴向屏幕投影的垂直方向；轴跟视线重合时退化成任意
	 * 方向（带缩成一点，没影响）。 */
	const vec3 view_dir = mat3(u_ViewMat) * (mat3(u_Local2WorldMat) * a_Dir);
	const vec2 screen_delta = (u_ProjectionMat * vec4(view_dir, 0.0f)).xy;
	const float screen_len = length(screen_delta);
	const vec2 screen_dir = screen_len > 1e-6f ? screen_delta / screen_len : vec2(1.0f, 0.0f);
	const vec2 screen_perp = vec2(-screen_dir.y, screen_dir.x);

	/* 展开量先乘 w 再加：透视除法后即恒定的像素偏移（1 像素 = 2 / 视口尺寸）。
	 * 网格与普通线的 a_Side = 0，展开量为 0、位置不变。 */
	const vec2 ndc_per_pixel = 2.0f / max(u_ViewportSize, vec2(1.0f));
	clip_pos.xy += screen_perp * (a_Side * max(u_LineWidth, 0.0f) * 0.5f) * ndc_per_pixel * clip_pos.w;
	gl_Position = clip_pos;

	v_Color = a_Color;
	v_WorldPos = world_pos.xyz;
	v_Side = a_Side;
	v_Dir = a_Dir;
}


#type fragment
#version 410 core

#include "builtin/Uniforms.glsl"

layout (location = 0) in vec3 v_Color;
layout (location = 1) in vec3 v_WorldPos;
layout (location = 2) noperspective in float v_Side;
layout (location = 3) in vec3 v_Dir;

/* 远景淡出范围（到相机的水平距离，世界单位）：[Inner, Outer] 之间线性渐隐。
 * 取值与 EditorBuiltinCamera 的网格常量同源，由材质浮点参数写入 —— 未设置时
 * smoothstep(0,0,·) 是未定义值，材质漏设会整片消失。 */
uniform float u_FadeInner;
uniform float u_FadeOuter;
/* 轴带宽度（物理像素）：与顶点着色器同源，用于把带边缘羽化 1 像素 */
uniform float u_LineWidth;

/* 网格的格距（世界单位）；0 = 当前绘制不是网格（坐标轴 / 实体 gizmo 不参与分类） */
uniform float u_GridMajor;

layout (location = 0) out vec4 OutFragColor;

/* 网格配色（都在世界空间里判身份，见 ClassifyGridColor） */
const vec3 kGridMinorColor = vec3(0.16f, 0.16f, 0.17f);
const vec3 kGridMajorColor = vec3(0.30f, 0.30f, 0.32f);
const vec3 kGridAxisColorX = vec3(0.50f, 0.10f, 0.10f);	/* z = 0 那条（沿 X 方向） */
const vec3 kGridAxisColorZ = vec3(0.10f, 0.10f, 0.50f);	/* x = 0 那条（沿 Z 方向） */

/* 网格线的身份按世界坐标分类：细线 / 每 u_GridMajor 格的主线 / 世界轴中线。按 patch 局部
 * 下标判会让亮线跟着视角滑动；这里用片元的世界坐标反推它落在哪条世界线上（线恒在整数坐标），
 * 跟 patch 平移无关。 */
vec3 ClassifyGridColor()
{
	const bool along_x = abs(v_Dir.x) > 0.5f;
	const float line_coord = floor((along_x ? v_WorldPos.z : v_WorldPos.x) + 0.5f);

	if (abs(line_coord) < 0.5f)
		return along_x ? kGridAxisColorX : kGridAxisColorZ;
	if (abs(line_coord - round(line_coord / u_GridMajor) * u_GridMajor) < 0.5f)
		return kGridMajorColor;
	return kGridMinorColor;
}

void main()
{
	/* 轴带边缘 1 像素羽化（抗锯齿）：按到带中心的屏幕距离给覆盖度。
	 * 网格与普通线的 v_Side 恒为 0，距离为 0、覆盖度恒为 1 —— 行为与之前一致。 */
	const float half_width = max(u_LineWidth, 1.0f) * 0.5f;
	const float edge_coverage = 1.0f - smoothstep(max(half_width - 1.0f, 0.0f), half_width, abs(v_Side) * half_width);

	/* 距离越远越透明：网格边缘平滑消失，不出现硬切的边界。
	 * 混合方式为 SrcAlpha / OneMinusSrcAlpha（见材质），渐隐段是"透出背景"
	 * 而不是把背景压黑。 */
	const float fade = 1.0f - smoothstep(u_FadeInner, u_FadeOuter, length(v_WorldPos.xz - u_ViewPos.xz));

	const vec3 color = u_GridMajor > 0.0f ? ClassifyGridColor() : v_Color;
	OutFragColor = vec4(color, fade * edge_coverage);
}
