#ifndef MATERIAL_COMMON_GLSL
#define MATERIAL_COMMON_GLSL

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
	float IOR;
	float ClearCoatThickness;
	float ClearCoatRoughness;
};

#endif // MATERIAL_COMMON_GLSL