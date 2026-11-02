#include "Pch.h"
#include "TerrainField.h"
#include "TerrainAsset.h"
#include "TerrainSampling.h"

#include <algorithm>
#include <filesystem>
#include <glm/gtc/type_ptr.hpp>

#include "Components.h"
#include "Helios/Common/PathUtils.h"
#include "Helios/Renderer/Frustum.h"
#include "Helios/VirtualDevice/DeviceTexture.h"
#include "Helios/VirtualDevice/DeviceVertexArray.h"

namespace Helios
{
	namespace
	{
		/* 单块地形的 patch 上限：参数填炸了也不至于把提交列表撑爆 */
		constexpr uint32_t kMaxPatches = 8192;

		/* morph 带宽缩放（见 TerrainField::SetGlobalMorphScale） */
		float s_MorphScale = 1.0f;

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

		/* 资产读取缓存：Rebuild 每帧都被调，按写入时间判断要不要重读。
		 * 文件不存在也记一行，免得每帧都去开一次必然失败的文件 */
		struct AssetEntry
		{
			std::string Path;
			std::filesystem::file_time_type WriteTime{};
			TerrainAsset::Data Data;
			bool Loaded{ false };
		};

		const TerrainAsset::Data* LoadAssetCached(const std::string& path,
			std::filesystem::file_time_type& out_write_time)
		{
			out_write_time = std::filesystem::file_time_type{};

			std::filesystem::path file_path;
			if (!TryPathFromUtf8(path, file_path))
				return nullptr;

			std::error_code error;
			const std::filesystem::file_time_type write_time =
				std::filesystem::last_write_time(file_path, error);
			const bool exists = !error;
			if (exists)
				out_write_time = write_time;

			static std::vector<AssetEntry> s_Cache;
			AssetEntry* entry = nullptr;
			for (AssetEntry& candidate : s_Cache)
			{
				if (candidate.Path == path)
				{
					entry = &candidate;
					break;
				}
			}

			if (entry != nullptr && entry->WriteTime == out_write_time && entry->Loaded == exists)
				return entry->Loaded ? &entry->Data : nullptr;

			if (entry == nullptr)
			{
				s_Cache.push_back(AssetEntry{});
				entry = &s_Cache.back();
			}

			entry->Path = path;
			entry->WriteTime = out_write_time;
			entry->Loaded = false;

			if (exists && TerrainAsset::Read(path, entry->Data))
				entry->Loaded = true;

			return entry->Loaded ? &entry->Data : nullptr;
		}
	}

	void TerrainField::Rebuild(const TerrainComponent& component, const glm::mat4& world)
	{
		PROFILE_FUNCTION();

		/* 绑了资产就从资产取高度；取不到退回程序化，场景不至于整块消失 */
		std::filesystem::file_time_type asset_write_time{};
		const TerrainAsset::Data* asset = component.m_AssetPath.empty()
			? nullptr : LoadAssetCached(component.m_AssetPath, asset_write_time);

		/* 构建键：参数、变换与资产都算进来，没变就整段跳过 */
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

		/* 资产内容换了（重新导出 / 外部编辑）也要重建，时间戳就是它的版本号 */
		const bool from_asset = asset != nullptr;
		if (from_asset)
		{
			mix(&asset_write_time, sizeof(asset_write_time));
			mix(component.m_AssetPath.data(), component.m_AssetPath.size());
		}

		if (key == m_BuildKey && m_HeightMap != nullptr)
			return;
		m_BuildKey = key;

		/* 范围按世界尺度展开：组件的局部半宽半长乘实体缩放 */
		const glm::vec3 axis_x(world[0]);
		const glm::vec3 axis_y(world[1]);
		const glm::vec3 axis_z(world[2]);
		const float scale_x = glm::length(axis_x);
		const float scale_y = glm::length(axis_y);
		const float scale_z = glm::length(axis_z);
		m_Origin = glm::vec3(world[3]);

		if (from_asset)
		{
			/* 数据以资产为准：组件上的分辨率 / 范围 / 起伏只在程序化模式下起作用 */
			m_Extent = glm::vec2(std::max(0.0f, asset->Extent.x * scale_x),
				std::max(0.0f, asset->Extent.y * scale_z));
			m_Resolution = std::clamp(asset->Resolution, 16u, 4096u);
			m_HeightScale = std::max(0.0f, asset->HeightScale * scale_y);
			m_LodRatio = std::max(0.5f, component.m_LodRatio);
			m_MinNodeSize = std::max(1u, component.m_MinNodeSize);
			m_AssetPath = component.m_AssetPath;
			UploadHeightMap(asset->Heights);
		}
		else
		{
			m_Extent = glm::vec2(std::max(0.0f, component.m_Extent.x * scale_x),
				std::max(0.0f, component.m_Extent.y * scale_z));
			m_Resolution = std::clamp(component.m_Resolution, 16u, 4096u);
			m_HeightScale = std::max(0.0f, component.m_HeightScale * scale_y);
			m_LodRatio = std::max(0.5f, component.m_LodRatio);
			m_MinNodeSize = std::max(1u, component.m_MinNodeSize);
			m_AssetPath.clear();
			BuildHeightMap(component);
		}

		/* 高度图与范围写进材质参数：属性面板里直接看到这块地形用的是哪张图。
		 * 资产材质（不是实例）不写，免得改到别人共用的那份 */
		const SharedPtr<Material>& material = component.m_MaterialSlot.pMaterial;
		if (material != nullptr && (component.m_MaterialSlot.IsInstance
			|| component.m_MaterialSlot.SourceAssetPath.empty()))
		{
			material->SetTexture("u_HeightMap", m_HeightMap);
			material->SetParameters(ParamType::Vec2, "u_TerrainExtent", m_Extent);
			material->SetParameters(ParamType::Vec2, "u_HeightmapSize",
				glm::vec2(static_cast<float>(m_Resolution)));
			material->SetParameters(ParamType::Float, "u_HeightScale", m_HeightScale);
		}
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

		/* 整体下移到最低点为 0：实体位置就是地面最低处，物件摆在实体高度上不会浮空。
		 * 只平移不缩放，起伏形态与幅度都不变 */
		const auto lowest = std::min_element(heights.begin(), heights.end());
		/* 先把最小值取出来再减：循环会改到最小值那一格本身，拿着迭代器反复读的话，
		 * 读到它就是 0 了，后面的行会少减一截，地形上出现一条随 seed 移动的台阶 */
		if (lowest != heights.end() && *lowest > 0.0f)
		{
			const float lowest_value = *lowest;
			for (float& h : heights)
				h -= lowest_value;
		}

		UploadHeightMap(std::move(heights));
	}

	void TerrainField::UploadHeightMap(std::vector<float> heights)
	{
		const uint32_t resolution = m_Resolution;

		/* 高度图存归一化值（0..1），世界高度 = 实体高度 + 值 × HeightScale */
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

		/* 换了高度数据，页池里的旧页全作废（页池本身留着，下一帧重烘） */
		m_VirtualTexture.Reset();

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
			return m_Origin.y;
		return m_Origin.y + *std::min_element(m_Heights.begin(), m_Heights.end()) * m_HeightScale;
	}

	float TerrainField::GetMaxWorldHeight() const
	{
		if (m_Heights.empty())
			return m_Origin.y + m_HeightScale;
		return m_Origin.y + *std::max_element(m_Heights.begin(), m_Heights.end()) * m_HeightScale;
	}

	bool TerrainField::GetHeightRange(const glm::vec2& min_xz, const glm::vec2& max_xz,
		float& out_min, float& out_max, uint32_t max_samples) const
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

		/* 范围估计（级联深度板）不必逐纹素扫：按每轴的最大采样数定步长 */
		int step = 1;
		if (max_samples > 0)
		{
			const int axis = std::max(texel_max.x - texel_min.x + 1, texel_max.y - texel_min.y + 1);
			step = std::max(1, (axis + static_cast<int>(max_samples) - 1) / static_cast<int>(max_samples));
		}

		float lo = std::numeric_limits<float>::max();
		float hi = -std::numeric_limits<float>::max();
		for (int z = texel_min.y; z <= texel_max.y; z += step)
		{
			for (int x = texel_min.x; x <= texel_max.x; x += step)
			{
				const float h = m_Heights[static_cast<size_t>(z) * resolution + static_cast<uint32_t>(x)];
				lo = std::min(lo, h);
				hi = std::max(hi, h);
			}
		}

		/* 步长跳过的最后一行一列补上，矩形边界上的峰不能被漏掉 */
		if (step > 1)
		{
			for (int x = texel_min.x; x <= texel_max.x; x += step)
			{
				const float h = m_Heights[static_cast<size_t>(texel_max.y) * resolution + static_cast<uint32_t>(x)];
				lo = std::min(lo, h);
				hi = std::max(hi, h);
			}
			for (int z = texel_min.y; z <= texel_max.y; z += step)
			{
				const float h = m_Heights[static_cast<size_t>(z) * resolution + static_cast<uint32_t>(texel_max.x)];
				lo = std::min(lo, h);
				hi = std::max(hi, h);
			}
		}
		if (lo > hi)
			return false;
		out_min = m_Origin.y + lo * m_HeightScale;
		out_max = m_Origin.y + hi * m_HeightScale;
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

		/* 根节点的包围盒盖住 0..HeightScale 这段：中心抬半高 */
		const glm::vec3 root_center(m_Origin.x, m_Origin.y + m_HeightScale * 0.5f, m_Origin.z);
		CollectNodes(&frustum, eye, 0.0f, root_center, root_size, 0, out);
		BalancePatches(out);
		MarkMorphPatches(out);
	}

	void TerrainField::BuildPatchesAround(const glm::vec3& center, float radius,
		std::vector<TerrainPatch>& out) const
	{
		if (m_HeightMap == nullptr || radius <= 0.0f)
			return;

		const float root_size = std::max(m_Extent.x, m_Extent.y) * 2.0f;
		if (root_size <= 0.0f)
			return;

		const glm::vec3 root_center(m_Origin.x, m_Origin.y + m_HeightScale * 0.5f, m_Origin.z);
		CollectNodes(nullptr, center, radius, root_center, root_size, 0, out);
		BalancePatches(out);
		MarkMorphPatches(out);
	}

	void TerrainField::CollectNodes(const Frustum* frustum, const glm::vec3& eye, float max_distance,
		const glm::vec3& center, float size, uint32_t lod, std::vector<TerrainPatch>& out) const
	{
		if (out.size() >= kMaxPatches)
			return;

		/* 剔除：包围盒含高度范围，只按 XZ 矩形判会把山体切掉 */
		const glm::vec3 extent(size * 0.5f, std::max(m_HeightScale * 0.5f, 0.01f), size * 0.5f);
		if (frustum != nullptr && frustum->IsOutside(center, extent))
			return;

		const float distance = glm::length(eye - center);
		/* 球范围（探针捕获）：节点自己的半对角线算进去，边角节点的叶子可能还伸进范围里 */
		if (max_distance > 0.0f && distance - size * 0.7072f > max_distance)
			return;

		const bool can_split = size > static_cast<float>(m_MinNodeSize) * 1.01f;
		if (can_split && distance < size * m_LodRatio)
		{
			const float child_size = size * 0.5f;
			const float offset = size * 0.25f;
			for (int quadrant = 0; quadrant < 4; ++quadrant)
			{
				const float dx = (quadrant & 1) ? offset : -offset;
				const float dz = (quadrant & 2) ? offset : -offset;
				CollectNodes(frustum, eye, max_distance, center + glm::vec3(dx, 0.0f, dz),
					child_size, lod + 1, out);
			}
			return;
		}

		TerrainPatch patch;
		patch.Info = glm::vec4(center.x - size * 0.5f, 0.0f, center.z - size * 0.5f, size);
		/* 采样步长：节点边长除以 32 段，粗 LOD 下让法线重建也跟着放粗 */
		patch.Lod = glm::vec4(static_cast<float>(lod), size / static_cast<float>(kPatchQuads), 0.0f, 0.0f);
		out.push_back(patch);
	}

	/* 四叉树没有 2:1 平衡：相邻叶子可能差两档以上，morph 只能对上差一档的情况。
	 * 这里做一遍平衡——邻居比自己细两档以上就把它拆一层，循环到稳定为止。 */
	void TerrainField::BalancePatches(std::vector<TerrainPatch>& patches)
	{
		const auto contains = [](const TerrainPatch& patch, const glm::vec2& point)
		{
			const glm::vec2 min(patch.Info.x, patch.Info.z);
			const glm::vec2 max = min + patch.Info.w;
			return point.x >= min.x && point.x < max.x && point.y >= min.y && point.y < max.y;
		};

		const auto split = [](const TerrainPatch& patch, std::vector<TerrainPatch>& out)
		{
			const float child = patch.Info.w * 0.5f;
			const glm::vec2 min(patch.Info.x, patch.Info.z);
			for (int quadrant = 0; quadrant < 4; ++quadrant)
			{
				TerrainPatch child_patch;
				const float x0 = min.x + ((quadrant & 1) ? child : 0.0f);
				const float z0 = min.y + ((quadrant & 2) ? child : 0.0f);
				child_patch.Info = glm::vec4(x0, 0.0f, z0, child);
				child_patch.Lod = glm::vec4(patch.Lod.x + 1.0f, child / static_cast<float>(kPatchQuads), 0.0f, 0.0f);
				out.push_back(child_patch);
			}
		};

		constexpr float kProbe = 0.01f;
		constexpr int kMaxIterations = 8;
		for (int iteration = 0; iteration < kMaxIterations; ++iteration)
		{
			int split_index = -1;
			for (size_t index = 0; index < patches.size() && split_index < 0; ++index)
			{
				const TerrainPatch& patch = patches[index];
				const glm::vec2 min(patch.Info.x, patch.Info.z);
				const glm::vec2 max = min + patch.Info.w;
				const float mid_x = (min.x + max.x) * 0.5f;
				const float mid_z = (min.y + max.y) * 0.5f;
				const glm::vec2 probes[4] = {
					{ min.x - kProbe, mid_z }, { max.x + kProbe, mid_z },
					{ mid_x, min.y - kProbe }, { mid_x, max.y + kProbe },
				};

				for (const glm::vec2& probe : probes)
				{
					for (size_t other = 0; other < patches.size(); ++other)
					{
						if (other == index)
							continue;
						/* 邻居比自己细两档以上：拆邻居，而不是拆自己（保持细节分布） */
						if (patches[other].Info.w > patch.Info.w * 2.01f && contains(patches[other], probe))
						{
							split_index = static_cast<int>(other);
							break;
						}
					}
					if (split_index >= 0)
						break;
				}
			}

			if (split_index < 0)
				break;

			const TerrainPatch victim = patches[static_cast<size_t>(split_index)];
			patches.erase(patches.begin() + split_index);
			split(victim, patches);
		}
	}

	/* 逐条边标记：哪一侧挨着**更粗**的邻居，那一条边就 morch 到父级表面。
	 * 同层邻居两边都不 morph（细表面本来就一模一样），只有细的一侧收敛 —— 两边都收
	 * 会收敛到不同精度的表面，交界反而裂成一道墙。邻居少时两两比对，太多时放弃（同层一致）。 */
	void TerrainField::MarkMorphPatches(std::vector<TerrainPatch>& patches)
	{
		constexpr size_t kMaxPatchForPairing = 1024;
		if (patches.size() > kMaxPatchForPairing)
			return;

		const auto contains = [](const TerrainPatch& patch, const glm::vec2& point)
		{
			const glm::vec2 min(patch.Info.x, patch.Info.z);
			const glm::vec2 max = min + patch.Info.w;
			return point.x >= min.x && point.x < max.x && point.y >= min.y && point.y < max.y;
		};

		constexpr float kProbe = 0.01f;
		for (TerrainPatch& patch : patches)
		{
			patch.Lod.z = 0.0f;
			if (patch.Lod.x < 0.5f)
				continue;	/* 根节点没有父级 */

			const glm::vec2 min(patch.Info.x, patch.Info.z);
			const glm::vec2 max = min + patch.Info.w;
			const float mid_x = (min.x + max.x) * 0.5f;
			const float mid_z = (min.y + max.y) * 0.5f;
			/* 位序与 TerrainCommon.glsl 一致：1 = -X，2 = +X，4 = -Z，8 = +Z */
			const glm::vec2 probes[4] = {
				{ min.x - kProbe, mid_z }, { max.x + kProbe, mid_z },
				{ mid_x, min.y - kProbe }, { mid_x, max.y + kProbe },
			};

			int mask = 0;
			for (int side = 0; side < 4; ++side)
			{
				for (const TerrainPatch& other : patches)
				{
					if (&other == &patch || other.Lod.x >= patch.Lod.x)
						continue;
					if (contains(other, probes[side]))
					{
						mask |= 1 << side;
						break;
					}
				}
			}

			patch.Lod.z = static_cast<float>(mask);
		}
	}

	void TerrainField::SetGlobalMorphScale(float scale)
	{
		s_MorphScale = std::max(0.0f, scale);
	}

	float TerrainField::GetGlobalMorphScale()
	{
		return s_MorphScale;
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
