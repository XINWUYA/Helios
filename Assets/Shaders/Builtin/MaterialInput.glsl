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
	float IOR;
	float ClearCoatThickness;
	float ClearCoatRoughness;
};

#endif // MATERIAL_INPUT_GLSL