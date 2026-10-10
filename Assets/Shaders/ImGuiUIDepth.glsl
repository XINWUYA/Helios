#type vertex
#version 410 core
layout(location = 0) in vec2 a_Position;
layout(location = 1) in vec2 a_TexCoord;
layout(location = 2) in vec4 a_Color;

layout(location = 0) out vec2 v_TexCoord;
layout(location = 1) out vec4 v_Color;

layout(std140, binding = 4) uniform UIUniformBuffer
{
    mat4 u_Projection;
};

void main()
{
    gl_Position = u_Projection * vec4(a_Position, 0.0, 1.0);
    v_TexCoord = a_TexCoord;
    v_Color = a_Color;
}

#type fragment
// @depth-texture u_DepthTexture
#version 410 core
layout(location = 0) in vec2 v_TexCoord;
layout(location = 1) in vec4 v_Color;

layout(location = 0) out vec4 FragColor;

/* 深度图显示的 ImGui 变体（Frame Graph 页的深度附件预览）。被标注声明的采样器在 MSL 端会
 * 生成 depth2d<float>；只取 .r 手动复制成灰度，Metal / OpenGL 两端输出一致。 */
layout(binding = 0) uniform sampler2D u_DepthTexture;

/* 显示范围（写过像素的 [min, max]，抓取端按层刷新写入预览纹理；见 FrameGraphCapture）。
 * max <= min（空层 / 常量层 / 未登记）时回退提亮曲线。 */
uniform float u_DepthMin;
uniform float u_DepthMax;

void main()
{
    float depth = texture(u_DepthTexture, v_TexCoord).r;
    /* 逐层归一化把该层内容的值域拉满：点光 / 聚光的透视反 Z 深度值域极小（近 0.05 / 远 =
     * 灯光 Range 时典型内容只有 1e-4 ~ 0.05），线性或固定提亮都拉不开对比；归一化后近处亮、
     * 远处渐暗、清屏 0 保持黑 —— 观感对齐 Xcode 抓帧的深度显示。无有效范围时沿用 ^1/4 提亮
     * （正交级联 / 未抓取登记的深度纹理）。 */
    float display;
    if (u_DepthMax > u_DepthMin)
        display = clamp((depth - u_DepthMin) / (u_DepthMax - u_DepthMin), 0.0, 1.0);
    else
        display = pow(clamp(depth, 0.0, 1.0), 0.25);
    FragColor = v_Color * vec4(display, display, display, 1.0);
}
