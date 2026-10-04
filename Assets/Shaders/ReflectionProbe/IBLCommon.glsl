#ifndef IBLCOMMON_GLSL
#define IBLCOMMON_GLSL

/* 根据 faceId 和面内坐标 uv(∈[-1,1]) 算采样方向。映射必须跟 GPU 立方体贴图的固定采样约定
 * 逐面一致（跟 ReflectionProbe::GetCaptureViewMatrix 的六个捕获相机朝向对应）。 */
vec3 GetCubeFaceDirection(int faceId, vec2 uv)
{
	vec3 dir = vec3(0.0f);
	switch (faceId)
	{
	case 0: dir = vec3( 1.0f, -uv.y, -uv.x); break; // +X
	case 1: dir = vec3(-1.0f, -uv.y,  uv.x); break; // -X
	case 2: dir = vec3( uv.x,  1.0f,  uv.y); break; // +Y
	case 3: dir = vec3( uv.x, -1.0f, -uv.y); break; // -Y
	case 4: dir = vec3( uv.x, -uv.y,  1.0f); break; // +Z
	case 5: dir = vec3(-uv.x, -uv.y, -1.0f); break; // -Z
	default: break;
	}
	return normalize(dir);
}

float RadicalInverse_VdC(uint bits) 
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10; // / 0x100000000
}

vec2 Hammersley(uint i, uint N)
{
    return vec2(float(i)/float(N), RadicalInverse_VdC(i));
}

vec3 ImportanceSampleGGX(vec2 Xi, vec3 N, float roughness)
{
    float a = roughness*roughness;

    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a*a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta*cosTheta);

    // from spherical coordinates to cartesian coordinates
    vec3 H;
    H.x = cos(phi) * sinTheta;
    H.y = sin(phi) * sinTheta;
    H.z = cosTheta;

    // from tangent-space vector to world-space sample vector
    vec3 up        = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent   = normalize(cross(up, N));
    vec3 bitangent = cross(N, tangent);

    vec3 sampleVec = tangent * H.x + bitangent * H.y + N * H.z;
    return normalize(sampleVec);
}

#endif // IBLCOMMON_GLSL
