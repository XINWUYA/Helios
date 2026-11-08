#ifndef MATERIAL_LIT_GLSL
#define MATERIAL_LIT_GLSL

#include "Math.glsl"
#include "MaterialInput.glsl"
#include "Uniforms.glsl"

/* 标准 lit 材质的贴图契约：9 个采样器（材质转换 / 调试视图都按名字找），声明随本库走，
 * 前向材质着色器与延迟 GBuffer 自动同源。@property = 归材质管的参数（面板列、写进 .mtl），
 * @group = 面板上的小标题；引擎逐绘制给的不标。显示名 / 控件 / 范围 / 条件见 DeviceShader.h。 */
uniform sampler2D u_AlbedoTexture; // @group(Surface) @property
uniform sampler2D u_SpecularTexture; // @group(Surface) @property
uniform sampler2D u_NormalTexture; // @group(Detail) @property @variant(MTL_NORMAL)
uniform sampler2D u_BumpTexture; // @group(Detail) @property @variant(MTL_PARALLAX)
uniform sampler2D u_DisplacementTexture; // @group(Detail) @property
uniform sampler2D u_RoughnessTexture; // @group(Surface) @property
uniform sampler2D u_MetallicTexture; // @group(Surface) @property
uniform sampler2D u_EmissiveTexture; // @group(Surface) @property
uniform sampler2D u_AmbientTexture; // @group(Surface) @property

/* 主贴图 UV 缩放偏移（xy=缩放，zw=偏移）：所有材质贴图采样共用、缺省不变换 */
uniform vec4 u_AlbedoTilingOffset = vec4(1.0f, 1.0f, 0.0f, 0.0f); // @group(Surface) @property

/* 标准 lit 材质的采样装配：输入世界位置 / 主 UV / 顶点色 / 世界法线 / 世界切线，
 * 输出填好的 SMaterialInput。前向与延迟共用这唯一一份实现。 */
void CalculateLitMaterial(inout SMaterialInput mtl,
	vec3 world_position, vec2 uv, vec4 base_color, vec3 world_normal, vec3 world_tangent)
{
	vec3 N = normalize(world_normal);

	vec2 base_uv = uv * u_AlbedoTilingOffset.xy + u_AlbedoTilingOffset.zw;

#if MTL_PARALLAX || MTL_NORMAL
	/* TBN：导入网格的切线不保证与法线垂直，Gram-Schmidt 重新正交化 */
	vec3 T = normalize(world_tangent);
	T = normalize(T - dot(T, N) * N);
	mat3 TBN = mat3(T, cross(T, N), N);
#endif

	vec2 sample_uv = base_uv;
#if MTL_PARALLAX
	/* 视差映射：按高度图把采样 UV 沿视线方向偏移；掠射角下 view_dir.z 趋零，钳制防噪声放大。
	 * 关闭时没有高度图，偏移恒为 0，与默认黑图逐位一致 */
	vec3 view_dir = normalize(TBN * (u_ViewPos - world_position));
	float height = texture(u_BumpTexture, base_uv).x;
	sample_uv = base_uv - view_dir.xy / max(view_dir.z, 1e-3f) * height;
#endif

	/* Albedo 转线性空间后再乘顶点色（sRGB 解码漏了会整片发白，是历史漂移点） */
	const vec4 albedo_sample = texture(u_AlbedoTexture, sample_uv);
	mtl.Albedo = SrgbToLinear(albedo_sample.rgb) * base_color.rgb;
	mtl.Opacity = albedo_sample.a * base_color.a;

#if MTL_NORMAL
	vec3 tangent_normal = texture(u_NormalTexture, sample_uv).xyz * 2.0f - 1.0f;
	mtl.Normal = normalize(TBN * tangent_normal);
#else
	/* 默认法线图是 (128,128,255)，解码后带 1/255 的偏斜；直接用几何法线更准 */
	mtl.Normal = N;
#endif

	mtl.Roughness = texture(u_RoughnessTexture, sample_uv).r;
	mtl.Metallic = texture(u_MetallicTexture, sample_uv).r;
	mtl.Specular = texture(u_SpecularTexture, sample_uv).rgb;
	mtl.Emission = texture(u_EmissiveTexture, sample_uv).rgb;
	mtl.Ambient = texture(u_AmbientTexture, sample_uv).rgb;
	/* 材质体系暂未接入的通道：填中性值 */
	mtl.AO = 1.0f;
	mtl.Anisotropy = 0.0f;
	mtl.IOR = 0.0f;
	mtl.ClearCoatThickness = 0.0f;
	mtl.ClearCoatRoughness = 0.0f;
}

#endif // MATERIAL_LIT_GLSL
