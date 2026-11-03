#include "Pch.h"
#include "TerrainField.h"
#include "TerrainAsset.h"
#include "TerrainSampling.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <glm/gtc/type_ptr.hpp>

#include "Components.h"
#include "Helios/Common/PathUtils.h"
#include "Helios/Renderer/Frustum.h"
#include "Helios/Renderer/RenderAPI.h"
#include "Helios/Renderer/Renderer.h"
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

		/* GPU 选择开关：默认开，后端不支持计算着色器时用不上这条路（PrepareSelection 会返回 false） */
		bool s_GpuSelection = true;

		/* GPU 选择的参数块：与 TerrainSelect.glsl 的 SelectBlock 逐字节对应 */
		struct alignas(16) TerrainSelectParams
		{
			glm::mat4 Model{ 1.0f };
			glm::vec4 Planes[6]{};
			glm::vec4 Eye{ 0.0f };     /* xyz 相机位置，w 节点总数 */
			glm::vec4 Counts{ 0.0f };  /* x 根边长、y 最细边长、z LOD 比例、w 世界缩放 */
			glm::vec4 Extra{ 0.0f };   /* x 高度半高、y 最大级数 */
		};

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

		/* 变换每帧都要跟上，但不参与高度图的重建判断：
		 * 拖 gizmo 只更新矩阵，不再重新生成整张高度图 */
		if (std::memcmp(glm::value_ptr(world), glm::value_ptr(m_World), sizeof(glm::mat4)) != 0)
		{
			m_World = world;
			m_WorldInverse = glm::inverse(world);
			m_NormalMatrix = glm::transpose(m_WorldInverse);
			m_WorldScale = glm::vec3(glm::length(glm::vec3(world[0])),
				glm::length(glm::vec3(world[1])), glm::length(glm::vec3(world[2])));
			m_Origin = glm::vec3(world[3]);
		}

		/* 材质参数变了（层颜色 / 层贴图 / 阈值）：页池里烘的颜色已经过期，
		 * 页表整张作废，下一帧按需重烘。几何不动，不用重建高度图 */
		const uint32_t material_revision = component.GetMaterial() != nullptr
			? component.GetMaterial()->GetRevision() : 0u;
		if (material_revision != m_MaterialRevision)
		{
			m_MaterialRevision = material_revision;
			m_VirtualTexture.Reset();
		}

		/* 绑了资产就从资产取高度；取不到退回程序化，场景不至于整块消失 */
		std::filesystem::file_time_type asset_write_time{};
		const TerrainAsset::Data* asset = component.m_AssetPath.empty()
			? nullptr : LoadAssetCached(component.m_AssetPath, asset_write_time);

		/* 构建键：只算局部参数与资产，变换不影响高度数据 */
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

		if (from_asset)
		{
			/* 数据以资产为准：组件上的分辨率 / 范围 / 起伏只在程序化模式下起作用 */
			m_Extent = glm::vec2(std::max(0.0f, asset->Extent.x), std::max(0.0f, asset->Extent.y));
			m_Resolution = std::clamp(asset->Resolution, 16u, 4096u);
			m_HeightScale = std::max(0.0f, asset->HeightScale);
			m_LodRatio = std::max(0.5f, component.m_LodRatio);
			m_MinNodeSize = std::max(1u, component.m_MinNodeSize);
			m_AssetPath = component.m_AssetPath;
			UploadHeightMap(asset->Heights);
		}
		else
		{
			m_Extent = glm::vec2(std::max(0.0f, component.m_Extent.x), std::max(0.0f, component.m_Extent.y));
			m_Resolution = std::clamp(component.m_Resolution, 16u, 4096u);
			m_HeightScale = std::max(0.0f, component.m_HeightScale);
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

		const uint32_t seed = static_cast<uint32_t>(component.m_Seed);

		/* 三层不同尺度的 fbm 叠出大起伏 + 中景丘陵 + 细节，再抬一次方压出山谷。
		 * 噪声按局部坐标采样：平移或旋转实体时山形跟着整体走，不会被重新生成 */
		for (uint32_t z = 0; z < resolution; ++z)
		{
			for (uint32_t x = 0; x < resolution; ++x)
			{
				const glm::vec2 uv(static_cast<float>(x) / static_cast<float>(resolution - 1),
					static_cast<float>(z) / static_cast<float>(resolution - 1));
				const glm::vec2 local_xz = (uv * 2.0f - 1.0f) * m_Extent;

				float h = 0.62f * Fbm(local_xz * 0.0025f, seed, 4);
				h += 0.28f * Fbm(local_xz * 0.012f, seed + 17u, 5);
				h += 0.10f * Fbm(local_xz * 0.05f, seed + 31u, 3);
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
		/* 节点表只跟范围与最细边长有关，跟高度数据一起在这条路上重建 */
		BuildNodeTable();

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

	glm::vec2 TerrainField::GetWorldHalfExtent() const
	{
		/* 局部矩形经世界矩阵变换后在 xz 上的轴对齐半范围：
		 * 转 45 度时比直接乘轴长要大，页表与阴影要用这个才不会在角上留空 */
		return glm::vec2(std::abs(m_World[0][0]) * m_Extent.x + std::abs(m_World[2][0]) * m_Extent.y,
			std::abs(m_World[0][2]) * m_Extent.x + std::abs(m_World[2][2]) * m_Extent.y);
	}

	glm::vec2 TerrainField::WorldToLocal(const glm::vec2& world_xz, float world_y) const
	{
		const glm::vec4 local = m_WorldInverse * glm::vec4(world_xz.x, world_y, world_xz.y, 1.0f);
		return glm::vec2(local.x, local.z);
	}

	glm::vec2 TerrainField::ToLocalXZ(const glm::vec2& world_xz, float world_y) const
	{
		/* 俯仰旋转时高度会反过来影响局部 xz：初值按 world_y 算一次，再用采到的高度修正。
		 * 绕 Y 轴的地形第一遍就是精确解 */
		glm::vec2 local = WorldToLocal(world_xz, world_y);
		float height = 0.0f;
		if (TerrainSampling::SampleHeight(m_Heights.empty() ? nullptr : m_Heights.data(),
			m_Resolution, m_Extent, m_HeightScale, local, height))
		{
			/* 用采到的地表高度再算一遍：俯仰旋转时第二遍才落到真正的局部点上 */
			const float surface_y = (m_World * glm::vec4(local.x, height, local.y, 1.0f)).y;
			local = WorldToLocal(world_xz, surface_y);
		}
		return local;
	}

	glm::vec3 TerrainField::ToWorld(const glm::vec2& local_xz, float local_height) const
	{
		return glm::vec3(m_World * glm::vec4(local_xz.x, local_height, local_xz.y, 1.0f));
	}

	bool TerrainField::ContainsPoint(const glm::vec2& world_xz) const
	{
		return TerrainSampling::Contains(m_Heights.empty() ? nullptr : m_Heights.data(),
			m_Resolution, m_Extent, ToLocalXZ(world_xz));
	}

	bool TerrainField::SampleHeightWorld(const glm::vec2& world_xz, float& out_height) const
	{
		const glm::vec2 local_xz = ToLocalXZ(world_xz);
		float local_height = 0.0f;
		if (!TerrainSampling::SampleHeight(m_Heights.empty() ? nullptr : m_Heights.data(),
			m_Resolution, m_Extent, m_HeightScale, local_xz, local_height))
			return false;

		out_height = ToWorld(local_xz, local_height).y;
		return true;
	}

	bool TerrainField::SampleNormalWorld(const glm::vec2& world_xz, glm::vec3& out_normal) const
	{
		const glm::vec2 local_xz = ToLocalXZ(world_xz);
		glm::vec3 local_normal(0.0f);
		if (!TerrainSampling::SampleNormal(m_Heights.empty() ? nullptr : m_Heights.data(),
			m_Resolution, m_Extent, m_HeightScale, local_xz, local_normal))
			return false;

		out_normal = glm::normalize(glm::vec3(m_NormalMatrix * glm::vec4(local_normal, 0.0f)));
		return true;
	}

	void TerrainField::GetWorldHeightBounds(float& out_min, float& out_max) const
	{
		out_min = std::numeric_limits<float>::max();
		out_max = -std::numeric_limits<float>::max();
		for (int corner = 0; corner < 8; ++corner)
		{
			const glm::vec3 local((corner & 1) ? m_Extent.x : -m_Extent.x,
				(corner & 2) ? m_HeightScale : 0.0f,
				(corner & 4) ? m_Extent.y : -m_Extent.y);
			const float y = (m_World * glm::vec4(local, 1.0f)).y;
			out_min = std::min(out_min, y);
			out_max = std::max(out_max, y);
		}
	}

	float TerrainField::GetMinWorldHeight() const
	{
		if (m_Heights.empty())
			return GetOrigin().y;

		/* 高度数据最低是 0，取局部盒子下沿的世界 y；实体位置不会比它更高 */
		float lo = 0.0f;
		float hi = 0.0f;
		GetWorldHeightBounds(lo, hi);
		return std::min(lo, GetOrigin().y);
	}

	float TerrainField::GetMaxWorldHeight() const
	{
		if (m_Heights.empty())
			return GetOrigin().y + m_HeightScale;

		float lo = 0.0f;
		float hi = 0.0f;
		GetWorldHeightBounds(lo, hi);
		return hi;
	}

	bool TerrainField::GetHeightRange(const glm::vec2& min_xz, const glm::vec2& max_xz,
		float& out_min, float& out_max, uint32_t max_samples) const
	{
		const uint32_t resolution = m_Resolution;
		if (m_Heights.size() < static_cast<size_t>(resolution) * resolution || m_Extent.x <= 0.0f)
			return false;

		/* 世界矩形 → 局部矩形。带旋转时矩形四角变换后取包围盒，比实际范围略大，
		 * 但阴影级联的深度板只要盖住就行，不能漏 */
		glm::vec2 local_min(std::numeric_limits<float>::max());
		glm::vec2 local_max(-std::numeric_limits<float>::max());
		for (int corner = 0; corner < 4; ++corner)
		{
			const glm::vec2 world_corner((corner & 1) ? max_xz.x : min_xz.x,
				(corner & 2) ? max_xz.y : min_xz.y);
			const glm::vec2 local = WorldToLocal(world_corner);
			local_min = glm::min(local_min, local);
			local_max = glm::max(local_max, local);
		}

		/* 世界矩形完全在局部范围外 */
		if (local_max.x < -m_Extent.x || local_max.y < -m_Extent.y
			|| local_min.x > m_Extent.x || local_min.y > m_Extent.y)
			return false;

		/* 局部 xz → 纹素矩形，超出地形的部分夹到边界 */
		const auto to_texel = [this, resolution](const glm::vec2& local_xz)
		{
			const glm::vec2 uv = local_xz / (m_Extent * 2.0f) + 0.5f;
			const float max_index = static_cast<float>(resolution - 1);
			return glm::ivec2(static_cast<int>(std::clamp(uv.x * max_index, 0.0f, max_index)),
				static_cast<int>(std::clamp(uv.y * max_index, 0.0f, max_index)));
		};

		const glm::ivec2 texel_min = to_texel(local_min);
		const glm::ivec2 texel_max = to_texel(local_max);

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

		/* 局部高度区间压到世界 y：上下界各取一遍，旋转时 z 也会贡献到 y */
		float world_lo = std::numeric_limits<float>::max();
		float world_hi = -std::numeric_limits<float>::max();
		for (int corner = 0; corner < 4; ++corner)
		{
			const glm::vec2 local_local((corner & 1) ? local_max.x : local_min.x,
				(corner & 2) ? local_max.y : local_min.y);
			const float y_lo = ToWorld(local_local, lo * m_HeightScale).y;
			const float y_hi = ToWorld(local_local, hi * m_HeightScale).y;
			world_lo = std::min(world_lo, std::min(y_lo, y_hi));
			world_hi = std::max(world_hi, std::max(y_lo, y_hi));
		}
		out_min = world_lo;
		out_max = world_hi;
		return true;
	}

	void TerrainField::BuildPatches(const Frustum& frustum, const glm::vec3& eye,
		std::vector<TerrainPatch>& out) const
	{
		if (m_HeightMap == nullptr)
			return;

		/* 根节点盖住整块地形（局部空间），从它往下一层层细分 */
		const float root_size = std::max(m_Extent.x, m_Extent.y) * 2.0f;
		if (root_size <= 0.0f)
			return;

		/* 根节点在局部空间居中；视锥与距离都按世界中心算 */
		CollectNodes(&frustum, eye, 0.0f, glm::vec2(0.0f), root_size, 0, out);
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

		CollectNodes(nullptr, center, radius, glm::vec2(0.0f), root_size, 0, out);
		BalancePatches(out);
		MarkMorphPatches(out);
	}

	void TerrainField::BuildNodeTable()
	{
		m_NodeTable.clear();
		m_MaxNodeLevel = 0;

		const float root_size = std::max(m_Extent.x, m_Extent.y) * 2.0f;
		if (root_size <= 0.0f)
			return;

		/* 满四叉树按层摊平：第 L 层 4^L 个节点，边长 root / 2^L。
		 * 最细一档取 MinNodeSize，跟 CPU 的 can_split 用同一条判据 */
		const float min_node = std::max(1.0f, static_cast<float>(m_MinNodeSize));
		constexpr uint32_t kMaxLevel = 10; /* 最细一档 1024² 个节点，够 4km 配 4 米 */
		const float half_root = root_size * 0.5f;

		for (uint32_t level = 0; level <= kMaxLevel; ++level)
		{
			const uint32_t side = 1u << level;
			const float node_size = root_size / static_cast<float>(side);
			for (uint32_t z = 0; z < side; ++z)
			{
				for (uint32_t x = 0; x < side; ++x)
				{
					const glm::vec2 center(-half_root + (static_cast<float>(x) + 0.5f) * node_size,
						-half_root + (static_cast<float>(z) + 0.5f) * node_size);
					m_NodeTable.push_back(glm::vec4(center.x, center.y, node_size,
						static_cast<float>(level)));
				}
			}

			m_MaxNodeLevel = level;
			if (node_size <= min_node * 1.01f)
				break;
		}

		/* 表换了，显存那份下一帧重建 */
		++m_NodeTableRevision;
		m_NodeStorage = nullptr;
		m_NodeUploadedRevision = 0;
	}

	TerrainField::SelectionSlot* TerrainField::FindSelection(const RenderView* view)
	{
		for (SelectionSlot& slot : m_Selections)
		{
			if (slot.View == view)
				return &slot;
		}

		/* 没见过这个视图：顶掉最久没用过的那一份（Frame 越大越旧，从没用过的是最大值） */
		SelectionSlot* oldest = &m_Selections[0];
		for (SelectionSlot& slot : m_Selections)
		{
			if (slot.Frame > oldest->Frame)
				oldest = &slot;
		}
		oldest->View = view;
		oldest->Frame = std::numeric_limits<uint32_t>::max();
		return oldest;
	}

	const TerrainField::SelectionSlot* TerrainField::FindSelection(const RenderView* view) const
	{
		for (const SelectionSlot& slot : m_Selections)
		{
			if (slot.View == view)
				return &slot;
		}
		return nullptr;
	}

	bool TerrainField::PrepareSelection(const glm::vec4 (&planes)[6], const glm::vec3& eye, uint32_t frame,
		const RenderView* view)
	{
		if (!s_GpuSelection || view == nullptr || m_NodeTable.empty() || m_HeightMap == nullptr)
			return false;

		const SharedPtr<RenderAPI>& api = Renderer::GetRenderAPI();
		if (api == nullptr || !api->SupportsCompute())
			return false;

		const uint32_t node_bytes = static_cast<uint32_t>(m_NodeTable.size() * sizeof(glm::vec4));
		if (m_NodeStorage == nullptr)
			m_NodeStorage = DeviceStorageBuffer::Create("TerrainNodes", node_bytes);

		if (m_NodeStorage == nullptr)
			return false;

		if (m_NodeUploadedRevision != m_NodeTableRevision)
		{
			m_NodeStorage->SetData(m_NodeTable.data(), node_bytes);
			m_NodeUploadedRevision = m_NodeTableRevision;
		}

		SelectionSlot* slot = FindSelection(view);
		slot->Frame = std::numeric_limits<uint32_t>::max();

		/* 压实缓冲按 patch 上限预分配，参数块每帧重置：实例数从这里往上累加 */
		const uint32_t patch_bytes = kMaxPatches * static_cast<uint32_t>(sizeof(TerrainPatch));
		if (slot->Storage == nullptr)
		{
			slot->Storage = DeviceStorageBuffer::Create("TerrainSelectedPatches", patch_bytes);
			slot->ViewBuffer = slot->Storage != nullptr
				? DeviceVertexBuffer::CreateShared("TerrainSelectedView", slot->Storage, patch_bytes)
				: nullptr;
			slot->Args = DeviceStorageBuffer::Create("TerrainSelectArgs", 5u * sizeof(uint32_t));
			if (slot->Storage == nullptr || slot->ViewBuffer == nullptr || slot->Args == nullptr)
			{
				slot->Storage = nullptr;
				slot->ViewBuffer = nullptr;
				slot->Args = nullptr;
				return false;
			}
		}

		const uint32_t args[5] = { kPatchIndexCount, 0u, 0u, 0u, 0u };
		slot->Args->SetData(args, sizeof(args));

		TerrainSelectParams params;
		params.Model = m_World;
		for (int index = 0; index < 6; ++index)
			params.Planes[index] = planes[index];

		const float world_scale = std::max(m_WorldScale.x, std::max(m_WorldScale.y, m_WorldScale.z));
		params.Eye = glm::vec4(eye, static_cast<float>(m_NodeTable.size()));
		params.Counts = glm::vec4(std::max(m_Extent.x, m_Extent.y) * 2.0f,
			static_cast<float>(m_MinNodeSize), m_LodRatio, world_scale);
		params.Extra = glm::vec4(std::max(m_HeightScale * m_WorldScale.y * 0.5f, 0.01f),
			static_cast<float>(m_MaxNodeLevel), 0.0f, 0.0f);

		{
			static int s_DebugCount = 0;
			if (s_DebugCount < 2)
			{
				++s_DebugCount;
				std::printf("[terrain-select] nodes=%zu maxLevel=%u root=%.1f min=%.1f ratio=%.2f scale=%.2f "
					"height=%.1f eye=(%.0f %.0f %.0f) node3=(%.0f %.0f %.1f %.1f)\n",
					m_NodeTable.size(), m_MaxNodeLevel, params.Counts.x, params.Counts.y, params.Counts.z,
					params.Counts.w, params.Extra.x, eye.x, eye.y, eye.z,
					m_NodeTable[21].x, m_NodeTable[21].y, m_NodeTable[21].z, m_NodeTable[21].w);
			}
		}

		const bool dispatched = Renderer::DispatchTerrainSelect(m_NodeStorage, slot->Storage,
			slot->Args, &params, static_cast<uint32_t>(sizeof(params)),
			static_cast<uint32_t>(m_NodeTable.size()));
		if (dispatched)
			slot->Frame = frame;
		return dispatched;
	}

	bool TerrainField::HasSelection(uint32_t frame, const RenderView* view) const
	{
		const SelectionSlot* slot = FindSelection(view);
		return slot != nullptr && slot->Frame == frame && slot->ViewBuffer != nullptr;
	}

	const SharedPtr<DeviceVertexBuffer>& TerrainField::GetSelectedInstances(const RenderView* view) const
	{
		static const SharedPtr<DeviceVertexBuffer> s_Empty;
		const SelectionSlot* slot = FindSelection(view);
		return slot != nullptr ? slot->ViewBuffer : s_Empty;
	}

	const SharedPtr<DeviceStorageBuffer>& TerrainField::GetSelectedArgs(const RenderView* view) const
	{
		static const SharedPtr<DeviceStorageBuffer> s_Empty;
		const SelectionSlot* slot = FindSelection(view);
		return slot != nullptr ? slot->Args : s_Empty;
	}

	const SharedPtr<DeviceStorageBuffer>& TerrainField::GetSelectedPatches(const RenderView* view) const
	{
		static const SharedPtr<DeviceStorageBuffer> s_Empty;
		const SelectionSlot* slot = FindSelection(view);
		return slot != nullptr ? slot->Storage : s_Empty;
	}

	uint32_t TerrainField::GetSelectedPatchCount(const RenderView* view) const
	{
		const SelectionSlot* slot = FindSelection(view);
		if (slot == nullptr || slot->Args == nullptr)
			return 0;

		const auto* data = static_cast<const uint32_t*>(slot->Args->GetMappedData());
		return data != nullptr ? data[1] : 0;
	}

	void TerrainField::SetGpuSelectionEnabled(bool enabled)
	{
		s_GpuSelection = enabled;
	}

	bool TerrainField::IsGpuSelectionEnabled()
	{
		return s_GpuSelection;
	}

	void TerrainField::CollectNodes(const Frustum* frustum, const glm::vec3& eye, float max_distance,
		const glm::vec2& local_center, float local_size, uint32_t lod,
		std::vector<TerrainPatch>& out) const
	{
		if (out.size() >= kMaxPatches)
			return;

		/* 节点中心的世界位置与半径：缩放取三轴最大值，旋转后也盖得住 */
		const glm::vec3 world_center = glm::vec3(m_World
			* glm::vec4(local_center.x, 0.0f, local_center.y, 1.0f));
		const float max_scale = std::max(m_WorldScale.x, std::max(m_WorldScale.y, m_WorldScale.z));
		const float world_size = local_size * max_scale;

		/* 剔除：包围盒含高度范围，只按 XZ 矩形判会把山体切掉 */
		const glm::vec3 extent(world_size * 0.5f,
			std::max(m_HeightScale * m_WorldScale.y * 0.5f, 0.01f), world_size * 0.5f);
		if (frustum != nullptr && frustum->IsOutside(world_center, extent))
			return;

		const float distance = glm::length(eye - world_center);
		/* 球范围（探针捕获）：节点自己的半对角线算进去，边角节点的叶子可能还伸进范围里 */
		if (max_distance > 0.0f && distance - world_size * 0.7072f > max_distance)
			return;

		/* 最细节点边长与 LOD 半径都是局部米：放大后的地形在同样的世界距离下更细 */
		const bool can_split = local_size > static_cast<float>(m_MinNodeSize) * 1.01f;
		if (can_split && distance < world_size * m_LodRatio)
		{
			const float child_size = local_size * 0.5f;
			const float offset = local_size * 0.25f;
			for (int quadrant = 0; quadrant < 4; ++quadrant)
			{
				const float dx = (quadrant & 1) ? offset : -offset;
				const float dz = (quadrant & 2) ? offset : -offset;
				CollectNodes(frustum, eye, max_distance, local_center + glm::vec2(dx, dz),
					child_size, lod + 1, out);
			}
			return;
		}

		TerrainPatch patch;
		patch.Info = glm::vec4(local_center.x - local_size * 0.5f, 0.0f,
			local_center.y - local_size * 0.5f, local_size);
		/* 采样步长：节点边长除以 32 段，粗 LOD 下让法线重建也跟着放粗 */
		patch.Lod = glm::vec4(static_cast<float>(lod), local_size / static_cast<float>(kPatchQuads),
			0.0f, 0.0f);
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
