#type compute
#version 450 core

/* 草地 GPU 放置：一次派发把整片草地的叶片实例算出来。
 * 公式与 GrassField::BuildCpuInstances 一字不差（同一个 PCG 散列、同一套分布），
 * 所以开关切到 CPU 路径时草地长得完全一样。
 *
 * 块内株数是固定的，所以「总序号 ÷ 每块株数」直接就是块号，不用查表。
 * 实例数据按 ObjectInstanceData 排：矩阵 4 列 + 参数，共 5 个 vec4。 */

layout(local_size_x = 64) in;

layout(std140, binding = 4) uniform GrassPlacementBlock
{
	vec4 u_BasisRight;    /* xyz 叶片右向轴，w = x 轴缩放 */
	vec4 u_BasisUp;       /* xyz 上向轴，w = y 轴缩放 */
	vec4 u_BasisForward;  /* xyz 前向轴 */
	vec4 u_Origin;        /* xyz 场地原点 */
	vec4 u_Grid;          /* x 块数 x、y 块数 z、z 每块株数、w 种子 */
	vec4 u_Shape;         /* x 半宽、y 半长、z 块宽、w 块深 */
	vec4 u_Blade;         /* x 叶高、y 叶宽、z 接受阴影、w 总株数 */
	/* ---- 贴地：有地形时叶片落到地形表面、姿态对齐坡面 ---- */
	vec4 u_Terrain;       /* xyz 地形中心世界坐标、w 起伏总高差 */
	vec4 u_TerrainShape;  /* x 半宽、y 半长、z 高度图分辨率、w 是否有地形 */
	vec4 u_GrassShape;    /* x 贴面程度、y cos(最大坡度)、z 是否启用坡度过滤 */
};

/* 地形高度数据：与 TerrainField 的 CPU 副本同一份，归一化 0..1 */
layout(std430, binding = 2) readonly buffer TerrainHeights
{
	float u_Heights[];
};

layout(std430, binding = 1) writeonly buffer GrassInstanceBuffer
{
	vec4 u_Instances[];
};

const float kTwoPi = 6.2831853f;

/* 双线性采样地形高度（世界单位），与 C++ 侧 SampleHeightWorld 同口径 */
float TerrainHeightAt(vec2 world_xz)
{
	if (u_TerrainShape.w < 0.5f)
		return u_Terrain.y;

	const vec2 half_extent = max(u_TerrainShape.xy, vec2(1e-3f));
	vec2 uv = clamp((world_xz - u_Terrain.xz) / (half_extent * 2.0f) + 0.5f, vec2(0.0f), vec2(1.0f));

	const int width = max(int(u_TerrainShape.z), 2);
	const float max_index = float(width - 1);
	const vec2 texel = uv * max_index;
	const vec2 base = floor(texel);
	const vec2 frac = texel - base;
	const ivec2 i0 = ivec2(base);
	const ivec2 i1 = min(i0 + 1, ivec2(width - 1));

	const float h00 = u_Heights[i0.y * width + i0.x];
	const float h10 = u_Heights[i0.y * width + i1.x];
	const float h01 = u_Heights[i1.y * width + i0.x];
	const float h11 = u_Heights[i1.y * width + i1.x];
	const float h = mix(mix(h00, h10, frac.x), mix(h01, h11, frac.x), frac.y);
	return u_Terrain.y + h * u_Terrain.w;
}

/* 坡面法线：邻居差分，步长取两个纹素 */
vec3 TerrainNormalAt(vec2 world_xz)
{
	const float texel = (u_TerrainShape.x * 2.0f) / max(u_TerrainShape.z - 1.0f, 1.0f);
	const float step = max(texel * 2.0f, 0.01f);
	const float hl = TerrainHeightAt(world_xz - vec2(step, 0.0f));
	const float hr = TerrainHeightAt(world_xz + vec2(step, 0.0f));
	const float hd = TerrainHeightAt(world_xz - vec2(0.0f, step));
	const float hu = TerrainHeightAt(world_xz + vec2(0.0f, step));
	return normalize(vec3(hl - hr, 2.0f * step, hd - hu));
}

/* 与 C++ 侧 MixHash 完全一致：PCG 的输出散列 */
uint MixHash(uint value)
{
	value = value * 747796405u + 2891336453u;
	const uint word = ((value >> ((value >> 28u) + 4u)) ^ value) * 277803737u;
	return (word >> 22u) ^ word;
}

float HashUnit(uint base, uint channel)
{
	return float(MixHash(base + channel * 0x9e3779b9u) & 0xFFFFFFu) / float(0x1000000u);
}

void main()
{
	const uint instance = gl_GlobalInvocationID.x;
	if (instance >= uint(u_Blade.w))
		return;

	const uint per_chunk = uint(u_Grid.z);
	const uint chunks_x = uint(u_Grid.x);
	const uint chunk_index = instance / per_chunk;
	const uint blade_index = instance - chunk_index * per_chunk;
	const uint cx = chunk_index % chunks_x;
	const uint cz = chunk_index / chunks_x;
	const uint seed = uint(u_Grid.w);

	const uint hash_base = MixHash(blade_index ^ MixHash(cx ^ MixHash(cz ^ MixHash(seed))));

	const vec3 right_axis = u_BasisRight.xyz;
	const vec3 forward_axis = u_BasisForward.xyz;
	const float half_x = u_Shape.x;
	const float half_z = u_Shape.y;

	const float local_x = -half_x + (float(cx) + HashUnit(hash_base, 1u)) * u_Shape.z;
	const float local_z = -half_z + (float(cz) + HashUnit(hash_base, 2u)) * u_Shape.w;

	const float yaw = HashUnit(hash_base, 3u) * kTwoPi;
	const float height = u_Blade.x * u_BasisUp.w * (0.7f + 0.55f * HashUnit(hash_base, 4u));
	const float width = u_Blade.y * u_BasisRight.w * (0.75f + 0.5f * HashUnit(hash_base, 5u));

	vec3 position = u_Origin.xyz + right_axis * local_x + forward_axis * local_z;

	/* 贴地：叶根落到地形表面；坡太陡的株退化掉（不跳过，保持每块株数固定） */
	vec3 ground_normal = vec3(0.0f, 1.0f, 0.0f);
	float growth = 1.0f;
	if (u_TerrainShape.w > 0.5f)
	{
		position.y = TerrainHeightAt(position.xz);
		ground_normal = TerrainNormalAt(position.xz);
		if (u_GrassShape.z > 0.5f && ground_normal.y < u_GrassShape.y)
			growth = 0.0f;
	}

	/* 姿态对齐坡面：up 向法线插值，右/前向对 up 正交化（与 C++ 侧同一套公式） */
	const vec3 up_axis = normalize(mix(vec3(0.0f, 1.0f, 0.0f), ground_normal,
		clamp(u_GrassShape.x, 0.0f, 1.0f)));
	vec3 dir_right = right_axis * cos(yaw) + forward_axis * sin(yaw);
	dir_right = normalize(dir_right - up_axis * dot(dir_right, up_axis));
	const vec3 dir_face = cross(dir_right, up_axis);

	const uint base = instance * 5u;
	u_Instances[base + 0u] = vec4(dir_right * (width * growth), 0.0f);
	u_Instances[base + 1u] = vec4(up_axis * (height * growth), 0.0f);
	u_Instances[base + 2u] = vec4(dir_face * (width * growth), 0.0f);
	u_Instances[base + 3u] = vec4(position, 1.0f);
	u_Instances[base + 4u] = vec4(-1.0f, u_Blade.z, HashUnit(hash_base, 6u), HashUnit(hash_base, 7u));
}
