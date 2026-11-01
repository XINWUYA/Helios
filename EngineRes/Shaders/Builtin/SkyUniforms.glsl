#ifndef SKY_UNIFORMS_GLSL
#define SKY_UNIFORMS_GLSL

/* 天空常量：与 C++ 侧 SkyUniformData 逐成员对应（顺序 / 类型 / 16 字节对齐）。
 * 只有需要大气散射、日月或星空的着色器才 include 这一份，普通材质不声明这个块。 */
layout(std140, binding = 5) uniform SkyUniformBuffer
{
	vec4 u_SkySunDirection;   /* xyz 指向太阳，w 太阳角半径（弧度） */
	vec4 u_SkySunRadiance;    /* rgb 大气层外的阳光辐照度（已换算到引擎亮度量级），
	                           * a 盘面竖直压扁因子（大气折射，贴地平线时日面是扁的） */
	vec4 u_SkyMoonDirection;  /* xyz 指向月亮，w 月亮角半径（弧度） */
	vec4 u_SkyMoonRadiance;   /* rgb 月亮色（已按月相与高度衰减），a 盘面竖直压扁因子 */
	vec4 u_SkyMoonLightDirection; /* xyz 月亮看到的太阳方向（月面着色），w 保留 */
	vec4 u_SkyRayleigh;       /* xyz 瑞利散射系数（1/m），w 瑞利标高（m） */
	vec4 u_SkyMie;            /* x 米氏散射系数，y 米氏标高，z 各向异性 g，w 浑浊度缩放 */
	vec4 u_SkyPlanet;         /* x 行星半径，y 大气半径，z 星空亮度，w 曝光 */
	vec4 u_SkyStars;          /* xyz 天球极轴，w 星空旋转角（弧度） */
};

#endif // SKY_UNIFORMS_GLSL
