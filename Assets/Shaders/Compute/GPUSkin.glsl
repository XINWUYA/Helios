#type compute
#version 450 core

/* GPU 蒙皮：绑定顶点 + 权重 + 调色板 → 上屏顶点。
 * 顶点布局与 .mesh 一致（16 float）：位置 3 / 法线 3 / 颜色 4 / UV 3 / 切线 3。
 * 权重缓冲每顶点 8 个 float：4 个关节下标 + 4 个权重。 */

layout(local_size_x = 64) in;

/* 调色板：关节世界变换 × 绑定逆矩阵 */
layout(std140, binding = 0) uniform SkinPaletteBlock
{
	mat4 u_SkinMatrices[64];
};

/* 参数：顶点数 */
layout(std140, binding = 4) uniform SkinParamBlock
{
	uint u_VertexCount;
};

/* 绑定顶点（只读） */
layout(std430, binding = 1) readonly buffer BindVertexBuffer
{
	float u_BindVertices[];
};

/* 每顶点 4 关节 + 4 权重（只读） */
layout(std430, binding = 2) readonly buffer SkinWeightBuffer
{
	float u_SkinWeights[];
};

/* 上屏顶点（只写） */
layout(std430, binding = 3) writeonly buffer SkinnedVertexBuffer
{
	float u_SkinnedVertices[];
};

void main()
{
	const uint vertex = gl_GlobalInvocationID.x;
	if (vertex >= u_VertexCount)
		return;

	const uint base = vertex * 16u;
	const vec3 position = vec3(u_BindVertices[base + 0u], u_BindVertices[base + 1u], u_BindVertices[base + 2u]);
	const vec3 normal = vec3(u_BindVertices[base + 3u], u_BindVertices[base + 4u], u_BindVertices[base + 5u]);
	const vec3 tangent = vec3(u_BindVertices[base + 13u], u_BindVertices[base + 14u], u_BindVertices[base + 15u]);

	vec3 skinned_position = vec3(0.0);
	vec3 skinned_normal = vec3(0.0);
	vec3 skinned_tangent = vec3(0.0);
	float total = 0.0;

	const uint weight_base = vertex * 8u;
	for (int influence = 0; influence < 4; ++influence)
	{
		const float weight = u_SkinWeights[weight_base + 4u + uint(influence)];
		if (weight <= 0.0)
			continue;

		const int joint = int(u_SkinWeights[weight_base + uint(influence)] + 0.5);
		const mat4 matrix = u_SkinMatrices[joint];
		skinned_position += weight * (matrix * vec4(position, 1.0)).xyz;
		skinned_normal += weight * (matrix * vec4(normal, 0.0)).xyz;
		skinned_tangent += weight * (matrix * vec4(tangent, 0.0)).xyz;
		total += weight;
	}

	if (total > 0.0)
	{
		skinned_position /= total;
		skinned_normal = normalize(skinned_normal);
		skinned_tangent = normalize(skinned_tangent);
	}
	else
	{
		skinned_position = position;
		skinned_normal = normal;
		skinned_tangent = tangent;
	}

	u_SkinnedVertices[base + 0u] = skinned_position.x;
	u_SkinnedVertices[base + 1u] = skinned_position.y;
	u_SkinnedVertices[base + 2u] = skinned_position.z;
	u_SkinnedVertices[base + 3u] = skinned_normal.x;
	u_SkinnedVertices[base + 4u] = skinned_normal.y;
	u_SkinnedVertices[base + 5u] = skinned_normal.z;
	u_SkinnedVertices[base + 6u] = u_BindVertices[base + 6u];
	u_SkinnedVertices[base + 7u] = u_BindVertices[base + 7u];
	u_SkinnedVertices[base + 8u] = u_BindVertices[base + 8u];
	u_SkinnedVertices[base + 9u] = u_BindVertices[base + 9u];
	u_SkinnedVertices[base + 10u] = u_BindVertices[base + 10u];
	u_SkinnedVertices[base + 11u] = u_BindVertices[base + 11u];
	u_SkinnedVertices[base + 12u] = u_BindVertices[base + 12u];
	u_SkinnedVertices[base + 13u] = skinned_tangent.x;
	u_SkinnedVertices[base + 14u] = skinned_tangent.y;
	u_SkinnedVertices[base + 15u] = skinned_tangent.z;
}
