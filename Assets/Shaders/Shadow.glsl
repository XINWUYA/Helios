#type vertex
#version 410 core

layout(location = 0) in vec3 a_Position;

/* 当前阴影层对应的矩阵选择：
 *   >= 0：级联下标，取 u_LightViewProjectionMat 的对应槽位（方向光 CSM）；
 *   -1  ：点光/聚光单面，取 u_PunctualShadowMat（每层渲染前由 CPU 端作为材质参数下发）。 */
layout(location = 0) uniform int u_CascadeIndex;
layout(location = 1) uniform mat4 u_PunctualShadowMat;

#include "builtin/Uniforms.glsl"

void main()
{
    const mat4 light_view_projection_mat = (u_CascadeIndex >= 0)
        ? u_LightViewProjectionMat[u_CascadeIndex]
        : u_PunctualShadowMat;
    gl_Position = light_view_projection_mat * u_Local2WorldMat * vec4(a_Position, 1.0f);
}


#type fragment
#version 410 core

void main()
{
    // 深度值会自动写入深度附件
    // 这里不需要输出颜色
}
