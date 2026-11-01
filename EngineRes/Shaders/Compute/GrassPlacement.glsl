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
};

layout(std430, binding = 1) writeonly buffer GrassInstanceBuffer
{
	vec4 u_Instances[];
};

const float kTwoPi = 6.2831853f;

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

	const vec3 dir_right = right_axis * cos(yaw) + forward_axis * sin(yaw);
	const vec3 dir_face = -right_axis * sin(yaw) + forward_axis * cos(yaw);
	const vec3 position = u_Origin.xyz + right_axis * local_x + forward_axis * local_z;

	const uint base = instance * 5u;
	u_Instances[base + 0u] = vec4(dir_right * width, 0.0f);
	u_Instances[base + 1u] = vec4(0.0f, height, 0.0f, 0.0f);
	u_Instances[base + 2u] = vec4(dir_face * width, 0.0f);
	u_Instances[base + 3u] = vec4(position, 1.0f);
	u_Instances[base + 4u] = vec4(-1.0f, u_Blade.z, HashUnit(hash_base, 6u), HashUnit(hash_base, 7u));
}
