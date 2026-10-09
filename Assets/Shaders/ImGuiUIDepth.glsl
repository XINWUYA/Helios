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

void main()
{
    float depth = texture(u_DepthTexture, v_TexCoord).r;
    FragColor = v_Color * vec4(depth, depth, depth, 1.0);
}
