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

#endif // MATERIAL_INPUT_GLSL