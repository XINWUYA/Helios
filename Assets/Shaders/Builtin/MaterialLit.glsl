#ifndef MATERIAL_LIT_GLSL
#define MATERIAL_LIT_GLSL

#include "Math.glsl"
#include "MaterialInput.glsl"
#include "Uniforms.glsl"

/* 标准 lit 材质的贴图契约：9 个采样器（材质转换 / 调试视图都按名字找）。
 * 声明随本库走，前向材质着色器与延迟 GBuffer 自动同源。
 * @property = 归材质管的参数：材质面板列、写进 .mtl；引擎逐绘制给的不标。
 * 显示名 / 控件类型 / 范围 / 条件显示写在同一条注释里，写法见 DeviceShader.h 的 MaterialPropertySpec。 */
uniform sampler2D u_AlbedoTexture; // @property
uniform sampler2D u_SpecularTexture; // @property
uniform sampler2D u_NormalTexture; // @property
uniform sampler2D u_BumpTexture; // @property
uniform sampler2D u_DisplacementTexture; // @property
uniform sampler2D u_RoughnessTexture; // @property
uniform sampler2D u_MetallicTexture; // @property
uniform sampler2D u_EmissiveTexture; // @property
uniform sampler2D u_AmbientTexture; // @property

/* 主贴图 UV 缩放偏移（xy=缩放，zw=偏移）：所有材质贴图采样共用、缺省不变换 */
uniform vec4 u_AlbedoTilingOffset = vec4(1.0f, 1.0f, 0.0f, 0.0f); // @property

/* 标准 lit 材质的采样装配：输入世界位置 / 主 UV / 顶点色 / 世界法线 / 世界切线，
 * 输出填好的 SMaterialInput。前向与延迟共用这唯一一份实现。 */
void CalculateLitMaterial(inout SMaterialInput mtl,
	vec3 world_position, vec2 uv, vec4 base_color, vec3 world_normal, vec3 world_tangent)
{
	/* TBN：导入网格的切线不保证与法线垂直，Gram-Schmidt 重新正交化 */
	vec3 T = normalize(world_tangent);
	vec3 N = normalize(world_normal);
	T = normalize(T - dot(T, N) * N);
	mat3 TBN = mat3(T, cross(T, N), N);

	vec2 base_uv = uv * u_AlbedoTilingOffset.xy + u_AlbedoTilingOffset.zw;

	/* 视差映射：按高度图把采样 UV 沿视线方向偏移；掠射角下 view_dir.z 趋零，钳制防噪声放大 */
	vec3 view_dir = normalize(TBN * (u_ViewPos - world_position));
	float height = texture(u_BumpTexture, base_uv).x;
	vec2 parallax_uv = base_uv - view_dir.xy / max(view_dir.z, 1e-3f) * height;

	/* Albedo 转线性空间后再乘顶点色（sRGB 解码漏了会整片发白，是历史漂移点） */
	const vec4 albedo_sample = texture(u_AlbedoTexture, parallax_uv);
	mtl.Albedo = SrgbToLinear(albedo_sample.rgb) * base_color.rgb;
	mtl.Opacity = albedo_sample.a * base_color.a;

	vec3 tangent_normal = texture(u_NormalTexture, parallax_uv).xyz * 2.0f - 1.0f;
	mtl.Normal = normalize(TBN * tangent_normal);

	mtl.Roughness = texture(u_RoughnessTexture, parallax_uv).r;
	mtl.Metallic = texture(u_MetallicTexture, parallax_uv).r;
	mtl.Specular = texture(u_SpecularTexture, parallax_uv).rgb;
	mtl.Emission = texture(u_EmissiveTexture, parallax_uv).rgb;
	mtl.Ambient = texture(u_AmbientTexture, parallax_uv).rgb;
	/* 材质体系暂未接入的通道：填中性值 */
	mtl.AO = 1.0f;
	mtl.Anisotropy = 0.0f;
	mtl.IOR = 0.0f;
	mtl.ClearCoatThickness = 0.0f;
	mtl.ClearCoatRoughness = 0.0f;
}

#endif // MATERIAL_LIT_GLSL
