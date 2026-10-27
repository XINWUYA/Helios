#ifndef MATH_GLSL
#define MATH_GLSL

#define PI 3.14159265359f
#define INV_PI 0.31830988618f
#define INV_2PI 0.15915494f


vec3 SrgbToLinear(vec3 color)
{
	return pow(color, vec3(2.2f));
}

#endif // MATH_GLSL