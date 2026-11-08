#ifndef MATERIAL_INPUT_GLSL
#define MATERIAL_INPUT_GLSL

struct SMaterialInput
{
	vec3 Albedo;
	vec3 Normal;
	float Roughness;
	float Metallic;
	vec3 Specular;
	float Anisotropy;
	vec3 Emission;
	vec3 Ambient;
	float AO;
	/* 不透明度：albedo 贴图 alpha × 顶点色 alpha（不透明材质不参与混合） */
	float Opacity;
	float IOR;
	float ClearCoatThickness;
	float ClearCoatRoughness;
};

/* 只定了 albedo / 法线 / 粗糙度 / 金属度时用它补齐其余通道：
 * 简化档、地形、调试读数这些地方只要这几个量，剩下的按中性值填 */
SMaterialInput MakeMaterialInput(vec3 albedo, vec3 normal, float roughness, float metallic)
{
	SMaterialInput mtl;
	mtl.Albedo = albedo;
	mtl.Normal = normal;
	mtl.Roughness = roughness;
	mtl.Metallic = metallic;
	mtl.Specular = vec3(0.0f);
	mtl.Anisotropy = 0.0f;
	mtl.Emission = vec3(0.0f);
	mtl.Ambient = vec3(1.0f);
	mtl.AO = 1.0f;
	mtl.Opacity = 1.0f;
	mtl.IOR = 0.0f;
	mtl.ClearCoatThickness = 0.0f;
	mtl.ClearCoatRoughness = 0.0f;
	return mtl;
}

#endif // MATERIAL_INPUT_GLSL