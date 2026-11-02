#include "Pch.h"
#include "TerrainField.h"
#include "TerrainSampling.h"

#include <algorithm>
#include <glm/gtc/type_ptr.hpp>

#include "Components.h"
#include "Helios/Renderer/Frustum.h"
#include "Helios/VirtualDevice/DeviceTexture.h"
#include "Helios/VirtualDevice/DeviceVertexArray.h"

namespace Helios
{
	namespace
	{
		/* 单块地形的 patch 上限：参数填炸了也不至于把提交列表撑爆 */
		constexpr uint32_t kMaxPatches = 8192;

		/* PCG 散列：值噪声的格点随机值靠它，相邻格点才不会连成一片 */
		uint32_t MixHash(uint32_t value)
		{
			value = value * 747796405u + 2891336453u;
			const uint32_t word = ((value >> ((value >> 28u) + 4u)) ^ value) * 277803737u;
			return (word >> 22u) ^ word;
		}

		float HashUnit(int32_t x, int32_t z, uint32_t seed)
		{
			const uint32_t base = static_cast<uint32_t>(x) * 73856093u
				^ static_cast<uint32_t>(z) * 19349663u ^ seed * 83492791u;
			return static_cast<float>(MixHash(base) & 0xFFFFFFu) / static_cast<float>(0x1000000u);
		}

		float SmoothStep(float t)
		{
			return t * t * (3.0f - 2.0f * t);
		}

		/* 值噪声：格点随机值 + smoothstep 双线性插值，比梯度噪声省一半算力 */
		float ValueNoise(glm::vec2 p, uint32_t seed)
		{
			const glm::vec2 cell = glm::floor(p);
			const glm::vec2 frac = p - cell;
			const int32_t x = static_cast<int32_t>(cell.x);
			const int32_t z = static_cast<int32_t>(cell.y);

			const float v00 = HashUnit(x, z, seed);
			const float v10 = HashUnit(x + 1, z, seed);
			const float v01 = HashUnit(x, z + 1, seed);
			const float v11 = HashUnit(x + 1, z + 1, seed);

			const float u = SmoothStep(frac.x);
			const float v = SmoothStep(frac.y);
			return glm::mix(glm::mix(v00, v10, u), glm::mix(v01, v11, u), v);
		}

		/* 多倍频叠加，返回 0..1 */
		float Fbm(glm::vec2 p, uint32_t seed, int octaves)
		{
			float sum = 0.0f;
			float amplitude = 0.5f;
			float total = 0.0f;
			for (int i = 0; i < octaves; ++i)
			{
				sum += ValueNoise(p, seed + static_cast<uint32_t>(i) * 131u) * amplitude;
				total += amplitude;
				p *= 2.03f;
				amplitude *= 0.5f;
			}
			return total > 0.0f ? sum / total : 0.0f;
		}
	}

	void TerrainField::Rebuild(const TerrainComponent& component, const glm::mat4& world)
	{
		PROFILE_FUNCTION();

		/* 构建键：参数与变换都算进来，没变就整段跳过 */
		uint64_t key = 0xcbf29ce484222325ull;
		const auto mix = [&key](const void* data, size_t size)
		{
			const auto* bytes = static_cast<const uint8_t*>(data);
			for (size_t i = 0; i < size; ++i)
			{
				key ^= bytes[i];
				key *= 0x100000001b3ull;
			}
		};
		mix(&component.m_Extent, sizeof(component.m_Extent));
		mix(&component.m_Resolution, sizeof(component.m_Resolution));
		mix(&component.m_HeightScale, sizeof(component.m_HeightScale));
		mix(&component.m_Seed, sizeof(component.m_Seed));
		mix(&component.m_LodRatio, sizeof(component.m_LodRatio));
		mix(&component.m_MinNodeSize, sizeof(component.m_MinNodeSize));
		mix(glm::value_ptr(world), sizeof(glm::mat4));

		if (key == m_BuildKey && m_HeightMap != nullptr)
			return;
		m_BuildKey = key;

		/* 范围按世界尺度展开：组件的是局部半宽半长，缩放挂在实体变换上 */
		const glm::vec3 axis_x(world[0]);
		const glm::vec3 axis_y(world[1]);
		const glm::vec3 axis_z(world[2]);
		const float scale_x = glm::length(axis_x);
		const float scale_y = glm::length(axis_y);
		const float scale_z = glm::length(axis_z);
		m_Origin = glm::vec3(world[3]);
		m_Extent = glm::vec2(std::max(0.0f, component.m_Extent.x * scale_x),
			std::max(0.0f, component.m_Extent.y * scale_z));
		m_Resolution = std::clamp(component.m_Resolution, 16u, 4096u);
		m_HeightScale = std::max(0.0f, component.m_HeightScale * scale_y);
		m_LodRatio = std::max(0.5f, component.m_LodRatio);
		m_MinNodeSize = std::max(1u, component.m_MinNodeSize);

		BuildHeightMap(component);
	}

	void TerrainField::BuildHeightMap(const TerrainComponent& component)
	{
		const uint32_t resolution = m_Resolution;
		std::vector<float> heights(static_cast<size_t>(resolution) * resolution, 0.0f);

		const glm::vec2 size = m_Extent * 2.0f;
		const uint32_t seed = static_cast<uint32_t>(component.m_Seed);

		/* 三层不同尺度的 fbm 叠出大起伏 + 中景丘陵 + 细节，再抬一次方压出山谷 */
		for (uint32_t z = 0; z < resolution; ++z)
		{
			for (uint32_t x = 0; x < resolution; ++x)
			{
				const glm::vec2 uv(static_cast<float>(x) / static_cast<float>(resolution - 1),
					static_cast<float>(z) / static_cast<float>(resolution - 1));
				const glm::vec2 world_xz = glm::vec2(m_Origin.x, m_Origin.z)
					+ (uv * 2.0f - 1.0f) * m_Extent;

				float h = 0.62f * Fbm(world_xz * 0.0025f, seed, 4);
				h += 0.28f * Fbm(world_xz * 0.012f, seed + 17u, 5);
				h += 0.10f * Fbm(world_xz * 0.05f, seed + 31u, 3);
				h = std::clamp(h, 0.0f, 1.0f);
				h = std::pow(h, 1.35f);

				heights[static_cast<size_t>(z) * resolution + x] = h;
			}
		}

		/* 高度图存归一化值（0..1），世界高度 = 值 × HeightScale */
		TextureDesc desc;
		desc.Width = resolution;
		desc.Height = resolution;
		desc.MipLevels = 1;
		desc.Format = TextureFormat::R32F;
		desc.SamplerType = SamplerType::Sampler2D;
		desc.Usage = TextureUsage::Sampleable;

		m_HeightMap = DeviceTexture::Create("TerrainHeightMap", desc);
		if (m_HeightMap == nullptr)
		{
			CORE_LOG_ERROR("Failed to create terrain heightmap texture.");
			return;
		}

		PixelDesc pixel_desc;
		pixel_desc.Format = PixelFormat::R;
		pixel_desc.Type = PixelType::Float;
		m_HeightMap->SetData(heights.data(), pixel_desc);

		/* 留一份 CPU 副本：草地 / 物件贴地时按世界坐标双线性采样 */
		m_Heights = std::move(heights);
		/* 重新建了高度图，GPU 副本作废，下次要的时候重传 */
		m_HeightStorage = nullptr;
	}

	const SharedPtr<DeviceStorageBuffer>& TerrainField::GetHeightStorage() const
	{
		if (m_HeightStorage == nullptr && !m_Heights.empty())
		{
			m_HeightStorage = DeviceStorageBuffer::Create("TerrainHeights",
				m_Heights.data(), static_cast<uint32_t>(m_Heights.size() * sizeof(float)));
		}
		return m_HeightStorage;
	}

	bool TerrainField::ContainsPoint(const glm::vec2& world_xz) const
	{
		return TerrainSampling::Contains(m_Heights.empty() ? nullptr : m_Heights.data(),
			m_Resolution, m_Origin, m_Extent, world_xz);
	}

	bool TerrainField::SampleHeightWorld(const glm::vec2& world_xz, float& out_height) const
	{
		return TerrainSampling::SampleHeight(m_Heights.empty() ? nullptr : m_Heights.data(),
			m_Resolution, m_Origin, m_Extent, m_HeightScale, world_xz, out_height);
	}

	bool TerrainField::SampleNormalWorld(const glm::vec2& world_xz, glm::vec3& out_normal) const
	{
		return TerrainSampling::SampleNormal(m_Heights.empty() ? nullptr : m_Heights.data(),
			m_Resolution, m_Origin, m_Extent, m_HeightScale, world_xz, out_normal);
	}

	float TerrainField::GetMinWorldHeight() const
	{
		if (m_Heights.empty())
			return m_Origin.y - m_HeightScale * 0.5f;
		return m_Origin.y + (*std::min_element(m_Heights.begin(), m_Heights.end()) - 0.5f) * m_HeightScale;
	}

	float TerrainField::GetMaxWorldHeight() const
	{
		if (m_Heights.empty())
			return m_Origin.y + m_HeightScale * 0.5f;
		return m_Origin.y + (*std::max_element(m_Heights.begin(), m_Heights.end()) - 0.5f) * m_HeightScale;
	}

	bool TerrainField::GetHeightRange(const glm::vec2& min_xz, const glm::vec2& max_xz,
		float& out_min, float& out_max) const
	{
		const uint32_t resolution = m_Resolution;
		if (m_Heights.size() < static_cast<size_t>(resolution) * resolution || m_Extent.x <= 0.0f)
			return false;

		/* 世界 xz → 纹素矩形，超出地形的部分夹到边界 */
		const auto to_texel = [this, resolution](const glm::vec2& world_xz)
		{
			const glm::vec2 uv = (world_xz - glm::vec2(m_Origin.x, m_Origin.z)) / (m_Extent * 2.0f) + 0.5f;
			const float max_index = static_cast<float>(resolution - 1);
			return glm::ivec2(static_cast<int>(std::clamp(uv.x * max_index, 0.0f, max_index)),
				static_cast<int>(std::clamp(uv.y * max_index, 0.0f, max_index)));
		};

		const glm::vec2 uv_min = (min_xz - glm::vec2(m_Origin.x, m_Origin.z)) / (m_Extent * 2.0f) + 0.5f;
		const glm::vec2 uv_max = (max_xz - glm::vec2(m_Origin.x, m_Origin.z)) / (m_Extent * 2.0f) + 0.5f;
		if (uv_max.x < 0.0f || uv_max.y < 0.0f || uv_min.x > 1.0f || uv_min.y > 1.0f)
			return false;

		const glm::ivec2 texel_min = to_texel(min_xz);
		const glm::ivec2 texel_max = to_texel(max_xz);

		float lo = std::numeric_limits<float>::max();
		float hi = -std::numeric_limits<float>::max();
		for (int z = texel_min.y; z <= texel_max.y; ++z)
		{
			for (int x = texel_min.x; x <= texel_max.x; ++x)
			{
				const float h = m_Heights[static_cast<size_t>(z) * resolution + static_cast<uint32_t>(x)];
				lo = std::min(lo, h);
				hi = std::max(hi, h);
			}
		}

		if (lo > hi)
			return false;
		out_min = m_Origin.y + (lo - 0.5f) * m_HeightScale;
		out_max = m_Origin.y + (hi - 0.5f) * m_HeightScale;
		return true;
	}

	void TerrainField::BuildPatches(const Frustum& frustum, const glm::vec3& eye,
		std::vector<TerrainPatch>& out) const
	{
		if (m_HeightMap == nullptr)
			return;

		/* 根节点盖住整块地形，从它往下一层层细分 */
		const float root_size = std::max(m_Extent.x, m_Extent.y) * 2.0f;
		if (root_size <= 0.0f)
			return;

		/* 根节点以实体位置为中心：高度是 ±HeightScale/2，上下各一半 */
		const glm::vec3 root_center(m_Origin.x, m_Origin.y, m_Origin.z);
		CollectNodes(frustum, eye, root_center, root_size, 0, out);
	}

	void TerrainField::CollectNodes(const Frustum& frustum, const glm::vec3& eye, const glm::vec3& center,
		float size, uint32_t lod, std::vector<TerrainPatch>& out) const
	{
		if (out.size() >= kMaxPatches)
			return;

		/* 剔除：包围盒含高度范围，只按 XZ 矩形判会把山体切掉 */
		const glm::vec3 extent(size * 0.5f, std::max(m_HeightScale * 0.5f, 0.01f), size * 0.5f);
		if (frustum.IsOutside(center, extent))
			return;

		const float distance = glm::length(eye - center);
		const bool can_split = size > static_cast<float>(m_MinNodeSize) * 1.01f;
		if (can_split && distance < size * m_LodRatio)
		{
			const float child_size = size * 0.5f;
			const float offset = size * 0.25f;
			for (int quadrant = 0; quadrant < 4; ++quadrant)
			{
				const float dx = (quadrant & 1) ? offset : -offset;
				const float dz = (quadrant & 2) ? offset : -offset;
				CollectNodes(frustum, eye, center + glm::vec3(dx, 0.0f, dz), child_size, lod + 1, out);
			}
			return;
		}

		TerrainPatch patch;
		patch.Info = glm::vec4(center.x - size * 0.5f, 0.0f, center.z - size * 0.5f, size);
		/* 采样步长：节点边长除以 32 段，粗 LOD 下让法线重建也跟着放粗 */
		patch.Lod = glm::vec4(static_cast<float>(lod), size / static_cast<float>(kPatchQuads), 0.0f, 0.0f);
		out.push_back(patch);
	}

	const MeshPrimitive& TerrainField::GetPatchPrimitive()
	{
		/* 模板网格：XZ 平面上的网格，只有 UV。世界位置由实例属性给，高度在顶点着色器里采样。
		 * 比 patch 本体多一圈顶点（UV 从 -1/32 到 33/32），那一圈在 VS 里拉成裙边挡接缝。 */
		static const MeshPrimitive s_Primitive = []()
		{
			const int side = static_cast<int>(kPatchQuads) + 3; /* 32 段本体 + 两侧各一列 */
			const float inv_quads = 1.0f / static_cast<float>(kPatchQuads);
			std::vector<float> vertices;
			vertices.reserve(static_cast<size_t>(side) * side * 2);
			for (int z = 0; z < side; ++z)
			{
				for (int x = 0; x < side; ++x)
				{
					vertices.push_back((static_cast<float>(x) - 1.0f) * inv_quads);
					vertices.push_back((static_cast<float>(z) - 1.0f) * inv_quads);
				}
			}

			const int quads = side - 1;
			std::vector<uint16_t> indices;
			indices.reserve(static_cast<size_t>(quads) * quads * 6);
			for (int z = 0; z < quads; ++z)
			{
				for (int x = 0; x < quads; ++x)
				{
					const uint16_t a = static_cast<uint16_t>(z * side + x);
					const uint16_t b = static_cast<uint16_t>(a + 1);
					const uint16_t c = static_cast<uint16_t>(a + side);
					const uint16_t d = static_cast<uint16_t>(c + 1);
					indices.insert(indices.end(), { a, c, b, b, c, d });
				}
			}

			const auto vertex_buffer = DeviceVertexBuffer::Create("TerrainPatch_VertexBuffer",
				vertices.data(), static_cast<uint32_t>(vertices.size() * sizeof(float)));
			vertex_buffer->SetLayout(VertexBufferLayout{
				{ "a_Uv", BufferDataType::Float2 },
			});

			const auto index_buffer = IndexBuffer::Create("TerrainPatch_IndexBuffer", indices.data(),
				static_cast<uint32_t>(indices.size()), IndexType::UInt16);

			auto vertex_array = DeviceVertexArray::Create("TerrainPatch_VertexArray");
			vertex_array->AddVertexBuffer(vertex_buffer);
			vertex_array->SetIndexBuffer(index_buffer);
			return MeshPrimitive(vertex_array);
		}();
		return s_Primitive;
	}

	const VertexBufferLayout& TerrainField::GetInstanceLayout()
	{
		/* 与 Terrain.glsl 的 location 5/6 对应 */
		static const VertexBufferLayout s_Layout = {
			{ "a_PatchInfo", BufferDataType::Float4 },
			{ "a_PatchLod", BufferDataType::Float4 },
		};
		return s_Layout;
	}
}
