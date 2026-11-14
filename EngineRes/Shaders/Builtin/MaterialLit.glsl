#ifndef MATERIAL_LIT_GLSL
#define MATERIAL_LIT_GLSL

#include "Math.glsl"
#include "MaterialAlbedo.glsl"
#include "MaterialInput.glsl"
#include "Uniforms.glsl"

/* 标准 lit 材质的贴图契约：主贴图在 MaterialAlbedo.glsl，其余 9 个采样器在这里（材质转换 /
 * 调试视图都按名字找），声明随本库走，前向材质着色器与延迟 GBuffer 自动同源。
 * @property = 归材质管的参数（面板列、写进 .mtl），@group = 面板上的小标题；
 * 引擎逐绘制给的不标。显示名 / 控件 / 范围 / 条件见 DeviceShader.h。 */
uniform sampler2D u_SpecularTexture; // @group(Surface) @property
uniform sampler2D u_NormalTexture; // @group(Detail) @property @variant(MTL_NORMAL)
uniform sampler2D u_BumpTexture; // @group(Detail) @property @variant(MTL_PARALLAX)
uniform sampler2D u_DisplacementTexture; // @group(Detail) @property
uniform sampler2D u_RoughnessTexture; // @group(Surface) @property
uniform sampler2D u_MetallicTexture; // @group(Surface) @property
uniform sampler2D u_EmissiveTexture; // @group(Surface) @property
uniform sampler2D u_AmbientTexture; // @group(Surface) @property
uniform sampler2D u_AOTexture; // @group(Surface) @property @variant(MTL_AO)

/* 视差高度（米）：只有开视差档才读。按表面实际尺度换算，图集里各处 UV 密度不同也不会错位 */
uniform float u_ParallaxHeight = 0.05f; // @group(Detail) @property(Height) @range(0.0, 0.3)

/* 标准 lit 材质的采样装配：输入世界位置 / 主 UV / 顶点色 / 世界法线 / 世界切线，
 * 输出填好的 SMaterialInput。前向与延迟共用这唯一一份实现。 */
void CalculateLitMaterial(inout SMaterialInput mtl,
	vec3 world_position, vec2 uv, vec4 base_color, vec3 world_normal, vec3 world_tangent)
{
	vec3 N = normalize(world_normal);

	vec2 base_uv = ApplyAlbedoTiling(uv);

#if MTL_PARALLAX || MTL_NORMAL
	/* TBN：导入网格的切线不保证与法线垂直，Gram-Schmidt 重新正交化 */
	vec3 T = normalize(world_tangent);
	T = normalize(T - dot(T, N) * N);
	mat3 TBN = mat3(T, cross(T, N), N);
#endif

	vec2 sample_uv = base_uv;
#if MTL_PARALLAX
	/* 视差映射：视线转到切线空间（TBN 是切到世界，反着乘要转置）。
	 * 高度按米给，用 UV 对表面的导数换算成 UV 偏移，图集里各块密度不同也不会错位；
	 * 交点落在表面点往相机一侧，所以是加；z 加偏置压住掠射角放大 */
	vec3 view_ts = normalize(transpose(TBN) * (u_ViewPos - world_position));
	vec3 pos_dx = dFdx(world_position);
	vec3 pos_dy = dFdy(world_position);
	float uv_per_meter = 0.5f * (length(dFdx(base_uv)) / max(length(pos_dx - N * dot(pos_dx, N)), 1e-6f)
		+ length(dFdy(base_uv)) / max(length(pos_dy - N * dot(pos_dy, N)), 1e-6f));
	float height_m = (texture(u_BumpTexture, base_uv).r - 0.5f) * u_ParallaxHeight;
	view_ts.z += 0.42f;
	sample_uv = base_uv + height_m * uv_per_meter * (view_ts.xy / view_ts.z);
#endif

#if MTL_PARALLAX
	/* 采样点已经偏移，导数仍取未偏移的 UV，否则 mip 会被偏移量拉低、整片糊掉 */
	const vec2 uv_dx = dFdx(base_uv);
	const vec2 uv_dy = dFdy(base_uv);
	#define SAMPLE_MATERIAL(tex, coord) textureGrad(tex, coord, uv_dx, uv_dy)
#else
	#define SAMPLE_MATERIAL(tex, coord) texture(tex, coord)
#endif

	/* Albedo 转线性空间后再乘顶点色（sRGB 解码漏了会整片发白，是历史漂移点） */
	const vec4 albedo_sample = SAMPLE_MATERIAL(u_AlbedoTexture, sample_uv);
	mtl.Albedo = SrgbToLinear(albedo_sample.rgb) * base_color.rgb;
	mtl.Opacity = albedo_sample.a * base_color.a;

#if MTL_NORMAL
	vec3 tangent_normal = SAMPLE_MATERIAL(u_NormalTexture, sample_uv).xyz * 2.0f - 1.0f;
	mtl.Normal = normalize(TBN * tangent_normal);
#else
	/* 默认法线图是 (128,128,255)，解码后带 1/255 的偏斜；直接用几何法线更准 */
	mtl.Normal = N;
#endif

	mtl.Roughness = SAMPLE_MATERIAL(u_RoughnessTexture, sample_uv).r;
	mtl.Metallic = SAMPLE_MATERIAL(u_MetallicTexture, sample_uv).r;
	mtl.Specular = SAMPLE_MATERIAL(u_SpecularTexture, sample_uv).rgb;
	mtl.Emission = SAMPLE_MATERIAL(u_EmissiveTexture, sample_uv).rgb;
	mtl.Ambient = SAMPLE_MATERIAL(u_AmbientTexture, sample_uv).rgb;
	/* AO 是表面烘焙的低频遮蔽，不跟着视差偏移；没配 AO 图的材质按无遮蔽算 */
#if MTL_AO
	mtl.AO = SAMPLE_MATERIAL(u_AOTexture, base_uv).r;
#else
	mtl.AO = 1.0f;
#endif
#undef SAMPLE_MATERIAL

	/* 材质体系暂未接入的通道：填中性值 */
	mtl.Anisotropy = 0.0f;
	mtl.IOR = 0.0f;
	mtl.ClearCoatThickness = 0.0f;
	mtl.ClearCoatRoughness = 0.0f;
}

#endif // MATERIAL_LIT_GLSL
