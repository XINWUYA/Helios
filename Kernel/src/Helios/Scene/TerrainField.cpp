#include "Pch.h"
#include "TerrainField.h"
#include "TerrainAsset.h"
#include "TerrainSampling.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <glm/gtc/type_ptr.hpp>

#include "Components.h"
#include "Helios/Common/PathUtils.h"
#include "Helios/Renderer/Frustum.h"
#include "Helios/Renderer/RenderAPI.h"
#include "Helios/Renderer/Renderer.h"
#include "Helios/VirtualDevice/DeviceFrameBuffer.h"
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
			glm::vec4 Counts{ 0.0f };  /* x 根边长、y 最细一层边长、z LOD 比例、w 世界缩放 */
			glm::vec4 Extra{ 0.0f };   /* x / w 局部半宽半长、y 最大级数、z patch 上限 */
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

		/* 资产读取缓存：Rebuild 一帧会被调十来次，按写入时间判断要不要重读。
		 * 文件状态半秒查一次；十秒没人要的条目清掉，免得大资产一直占着内存。
		 * 文件不存在也记一行，免得每帧都去开一次必然失败的文件 */
		struct AssetEntry
		{
			std::string Path;
			std::filesystem::file_time_type WriteTime{};
			std::chrono::steady_clock::time_point CheckedAt{};
			std::chrono::steady_clock::time_point UsedAt{};
			TerrainAsset::Data Data;
			bool Loaded{ false };
		};

		const TerrainAsset::Data* LoadAssetCached(const std::string& path,
			std::filesystem::file_time_type& out_write_time)
		{
			using Clock = std::chrono::steady_clock;
			constexpr auto kCheckInterval = std::chrono::milliseconds(500);
			constexpr auto kKeepAlive = std::chrono::seconds(10);

			static std::vector<AssetEntry> s_Cache;
			const Clock::time_point now = Clock::now();
			std::erase_if(s_Cache, [&](const AssetEntry& entry)
			{
				return entry.Path != path && now - entry.UsedAt > kKeepAlive;
			});

			auto found = std::find_if(s_Cache.begin(), s_Cache.end(),
				[&path](const AssetEntry& entry) { return entry.Path == path; });
			if (found == s_Cache.end())
			{
				s_Cache.push_back(AssetEntry{});
				found = std::prev(s_Cache.end());
				found->Path = path;
				found->CheckedAt = now - kCheckInterval;
			}
			AssetEntry& entry = *found;
			entry.UsedAt = now;

			if (now - entry.CheckedAt >= kCheckInterval)
			{
				entry.CheckedAt = now;

				std::filesystem::path file_path;
				std::error_code error;
				std::filesystem::file_time_type write_time{};
				const bool exists = TryPathFromUtf8(path, file_path)
					&& (write_time = std::filesystem::last_write_time(file_path, error), !error);
				if (!exists)
					write_time = std::filesystem::file_time_type{};

				if (write_time != entry.WriteTime || !entry.Loaded)
				{
					entry.WriteTime = write_time;
					entry.Loaded = exists && TerrainAsset::Read(path, entry.Data);
				}
			}

			out_write_time = entry.WriteTime;
			return entry.Loaded ? &entry.Data : nullptr;
		}
	}

	void TerrainField::Rebuild(const TerrainComponent& component, const glm::mat4& world)
	{
		PROFILE_FUNCTION();

		/* 变换每帧都要跟上，但不参与高度图的重建判断：
		 * 拖 gizmo 只更新矩阵，不再重新生成整张高度图 */
		if (std::memcmp(glm::value_ptr(world), glm::value_ptr(m_World), sizeof(glm::mat4)) != 0)
		{
			/* 页网格锚在地形原点上，平移时页跟着走；旋转缩放改了世界到局部的映射，页与底图要重烘 */
			if (glm::mat3(world) != glm::mat3(m_World))
			{
				m_VirtualTexture.Reset();
				++m_TransformRevision;
			}
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

		/* 高度数据换了，贴地对象要重新采样；笔刷编辑只推进这一个版本号，不动构建键 */
		m_HeightsRevision = m_BuildKey;

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

		/* 留一份 CPU 副本：草地 / 物件贴地时按世界坐标双线性采样 */
		m_Heights = std::move(heights);
		m_HeightsEdited = false;
		/* 节点的高度范围要读 CPU 副本，放在它后面建 */
		BuildNodeTable();
		/* 重新建了高度图，GPU 副本作废，下次要的时候重传 */
		m_HeightStorage = nullptr;
	}

	void TerrainField::RefreshHeightMap(const glm::uvec2& texel_min, const glm::uvec2& texel_max)
	{
		if (m_HeightMap == nullptr || m_Heights.empty())
			return;

		PixelDesc pixel_desc;
		pixel_desc.Format = PixelFormat::R;
		pixel_desc.Type = PixelType::Float;
		m_HeightMap->SetData(m_Heights.data(), pixel_desc);
		UpdateNodeHeights(texel_min, texel_max);

		/* 高度变了：页池里的颜色与烘焙用的高度都过期，GPU 副本下次要的时候重传 */
		m_VirtualTexture.Reset();
		m_HeightStorage = nullptr;
		++m_HeightsRevision;
		m_HeightsEdited = true;
	}

	bool TerrainField::ApplyBrush(const glm::vec2& local_center, const glm::vec2& local_radius, float amount,
		BrushMode mode, glm::uvec2& out_min, glm::uvec2& out_max)
	{
		if (m_Heights.empty() || m_Resolution < 2 || local_radius.x <= 0.0f || local_radius.y <= 0.0f
			|| amount <= 0.0f)
			return false;

		const glm::vec2 extent = glm::max(m_Extent, glm::vec2(1e-3f));
		const glm::vec2 texel = extent * 2.0f / static_cast<float>(m_Resolution - 1);
		const float max_index = static_cast<float>(m_Resolution - 1);
		const auto to_index = [&](const glm::vec2& local)
		{
			return glm::uvec2(glm::clamp((local + extent) / texel, glm::vec2(0.0f), glm::vec2(max_index)));
		};
		const glm::uvec2 lo = to_index(local_center - local_radius);
		const glm::uvec2 hi = to_index(local_center + local_radius);

		/* 抹平读改动前的那份：边读边写会让先扫到的格子拖着后面的走，抹出方向性 */
		std::vector<float> source;
		glm::uvec2 source_min(0);
		uint32_t source_width = 0;
		if (mode == BrushMode::Smooth)
		{
			source_min = glm::uvec2(lo.x > 0 ? lo.x - 1 : 0, lo.y > 0 ? lo.y - 1 : 0);
			const glm::uvec2 source_max = glm::min(hi + 1u, glm::uvec2(m_Resolution - 1));
			source_width = source_max.x - source_min.x + 1;
			source.resize(static_cast<size_t>(source_width) * (source_max.y - source_min.y + 1));
			for (uint32_t z = source_min.y; z <= source_max.y; ++z)
			{
				std::copy_n(m_Heights.begin() + static_cast<size_t>(z) * m_Resolution + source_min.x, source_width,
					source.begin() + static_cast<size_t>(z - source_min.y) * source_width);
			}
		}
		const auto source_at = [&](uint32_t x, uint32_t z)
		{
			return source[static_cast<size_t>(z - source_min.y) * source_width + (x - source_min.x)];
		};

		const float to_normalized = 1.0f / std::max(m_HeightScale, 1e-3f);
		const float blend = std::clamp(amount, 0.0f, 1.0f);
		bool any = false;

		for (uint32_t z = lo.y; z <= hi.y; ++z)
		{
			for (uint32_t x = lo.x; x <= hi.x; ++x)
			{
				/* 半径按两轴分开给：非均匀缩放的地形上笔刷在世界里仍是圆 */
				const glm::vec2 local = -extent + glm::vec2(static_cast<float>(x), static_cast<float>(z)) * texel;
				const float distance = glm::length((local - local_center) / local_radius);
				if (distance >= 1.0f)
					continue;

				/* 平滑收边：中心满、边缘归零，扫过去不留硬边 */
				const float t = 1.0f - distance;
				const float weight = t * t * (3.0f - 2.0f * t);

				float& height = m_Heights[static_cast<size_t>(z) * m_Resolution + x];
				if (mode == BrushMode::Smooth)
				{
					/* 抹平：混向四邻域均值，amount 是混合比例 */
					const float average = 0.25f * (source_at(x > 0 ? x - 1 : x, z)
						+ source_at(x + 1 < m_Resolution ? x + 1 : x, z)
						+ source_at(x, z > 0 ? z - 1 : z)
						+ source_at(x, z + 1 < m_Resolution ? z + 1 : z));
					height = glm::mix(height, average, blend * weight);
				}
				else
				{
					const float delta = amount * to_normalized * weight;
					height += (mode == BrushMode::Raise) ? delta : -delta;
				}

				height = std::clamp(height, 0.0f, 1.0f);
				any = true;
			}
		}

		if (!any)
			return false;

		RefreshHeightMap(lo, hi);
		out_min = lo;
		out_max = hi;
		return true;
	}

	bool TerrainField::SampleHeightLocal(const glm::vec2& local_xz, float& out_height) const
	{
		return TerrainSampling::SampleHeight(m_Heights.empty() ? nullptr : m_Heights.data(),
			m_Resolution, m_Extent, m_HeightScale, local_xz, out_height);
	}

	void TerrainField::BeginBrushStroke()
	{
		m_StrokeSnapshot = m_Heights;
		m_StrokeActive = !m_StrokeSnapshot.empty();
	}

	bool TerrainField::EndBrushStroke(glm::uvec2& out_min, glm::uvec2& out_max,
		std::vector<float>& out_before, std::vector<float>& out_after)
	{
		const bool active = m_StrokeActive;
		m_StrokeActive = false;
		if (!active || m_StrokeSnapshot.size() != m_Heights.size())
		{
			m_StrokeSnapshot.clear();
			return false;
		}

		/* 找出真正改动过的纹素矩形：撤销只存这一块，不存整张高度图 */
		constexpr float kEpsilon = 1e-7f;
		uint32_t min_x = m_Resolution;
		uint32_t min_z = m_Resolution;
		uint32_t max_x = 0;
		uint32_t max_z = 0;
		for (uint32_t z = 0; z < m_Resolution; ++z)
		{
			for (uint32_t x = 0; x < m_Resolution; ++x)
			{
				const size_t index = static_cast<size_t>(z) * m_Resolution + x;
				if (std::fabs(m_Heights[index] - m_StrokeSnapshot[index]) <= kEpsilon)
					continue;
				min_x = std::min(min_x, x);
				max_x = std::max(max_x, x);
				min_z = std::min(min_z, z);
				max_z = std::max(max_z, z);
			}
		}

		if (min_x > max_x || min_z > max_z)
		{
			m_StrokeSnapshot.clear();
			return false;
		}

		const uint32_t width = max_x - min_x + 1;
		const uint32_t rows = max_z - min_z + 1;
		out_before.resize(static_cast<size_t>(width) * rows);
		out_after.resize(out_before.size());
		for (uint32_t z = 0; z < rows; ++z)
		{
			const size_t dst = static_cast<size_t>(z) * width;
			const size_t src = static_cast<size_t>(min_z + z) * m_Resolution + min_x;
			std::copy_n(m_StrokeSnapshot.begin() + src, width, out_before.begin() + dst);
			std::copy_n(m_Heights.begin() + src, width, out_after.begin() + dst);
		}

		m_StrokeSnapshot.clear();
		out_min = glm::uvec2(min_x, min_z);
		out_max = glm::uvec2(max_x, max_z);
		return true;
	}

	void TerrainField::WriteHeightRect(const glm::uvec2& min, const glm::uvec2& max,
		const std::vector<float>& values)
	{
		if (m_Heights.empty())
			return;

		const uint32_t x0 = std::min(std::min(min.x, max.x), m_Resolution - 1);
		const uint32_t x1 = std::min(std::max(min.x, max.x), m_Resolution - 1);
		const uint32_t z0 = std::min(std::min(min.y, max.y), m_Resolution - 1);
		const uint32_t z1 = std::min(std::max(min.y, max.y), m_Resolution - 1);
		const uint32_t width = x1 - x0 + 1;
		const uint32_t rows = z1 - z0 + 1;
		if (values.size() < static_cast<size_t>(width) * rows)
			return;

		for (uint32_t z = 0; z < rows; ++z)
		{
			const size_t src = static_cast<size_t>(z) * width;
			std::copy_n(values.begin() + src, width,
				m_Heights.begin() + static_cast<size_t>(z0 + z) * m_Resolution + x0);
		}

		RefreshHeightMap(glm::uvec2(x0, z0), glm::uvec2(x1, z1));
	}

	bool TerrainField::ExportAsset(const std::string& path, const std::string& material_path)
	{
		if (m_Heights.empty() || path.empty())
			return false;

		TerrainAsset::Data data;
		data.Resolution = m_Resolution;
		data.Extent = m_Extent;
		data.HeightScale = m_HeightScale;
		data.MaterialPath = material_path;
		data.Heights = m_Heights;
		if (!TerrainAsset::Write(path, data))
			return false;

		m_HeightsEdited = false;
		return true;
	}

	bool TerrainField::EnsureBasemap() const
	{
		if (m_Basemap != nullptr)
			return m_BasemapFrameBuffer != nullptr;

		TextureDesc desc;
		desc.Width = kBasemapSize;
		desc.Height = kBasemapSize;
		desc.MipLevels = 1;
		desc.Format = TextureFormat::RGBA8;
		desc.SamplerType = SamplerType::Sampler2D;
		desc.Usage = TextureUsage::ColorAttachment | TextureUsage::Sampleable;
		m_Basemap = DeviceTexture::Create("TerrainBasemap", desc);
		if (m_Basemap == nullptr)
			return false;

		RenderBufferInfo color_info;
		color_info.RenderTarget = m_Basemap;

		FrameBufferDesc frame_desc;
		frame_desc.Samples = 1;
		frame_desc.Usage = RenderBufferUsage::ColorDefault;
		frame_desc.ViewportRegion = ViewportRegion{ 0, 0, kBasemapSize, kBasemapSize };
		frame_desc.ColorRenderBuffers.push_back(color_info);
		m_BasemapFrameBuffer = DeviceFrameBuffer::Create("TerrainBasemapFrameBuffer", frame_desc);
		return m_BasemapFrameBuffer != nullptr;
	}

	glm::vec4 TerrainField::GetBasemapRect() const
	{
		if (m_Basemap == nullptr || m_HeightMap == nullptr)
			return glm::vec4(0.0f);

		/* 旋转过的地形按轴对齐包围盒盖住：底图是正方形，边长取两轴里大的那个 */
		const glm::vec2 world_half = GetWorldHalfExtent();
		const float half = std::max(world_half.x, world_half.y);
		if (half <= 0.0f)
			return glm::vec4(0.0f);

		const glm::vec2 min_corner(m_Origin.x - half, m_Origin.z - half);
		const float size = half * 2.0f;
		return glm::vec4(min_corner.x, min_corner.y, size, size / static_cast<float>(kBasemapSize));
	}

	bool TerrainField::NeedsBasemap(uint32_t material_revision) const
	{
		return m_BasemapHeights != m_HeightsRevision || m_BasemapMaterial != material_revision
			|| m_BasemapTransform != m_TransformRevision;
	}

	void TerrainField::MarkBasemapBaked(uint32_t material_revision) const
	{
		m_BasemapHeights = m_HeightsRevision;
		m_BasemapMaterial = material_revision;
		m_BasemapTransform = m_TransformRevision;
	}

	bool TerrainField::Raycast(const glm::vec3& origin, const glm::vec3& direction,
		glm::vec2& out_local, float& out_distance) const
	{
		if (m_Heights.empty() || m_HeightMap == nullptr)
			return false;

		/* 射线搬进局部空间：高度场、范围都在这一套坐标里 */
		const glm::vec3 local_origin = glm::vec3(m_WorldInverse * glm::vec4(origin, 1.0f));
		const glm::vec3 local_dir_raw = glm::vec3(m_WorldInverse * glm::vec4(direction, 0.0f));
		const float dir_length = glm::length(local_dir_raw);
		if (dir_length <= 1e-6f)
			return false;
		const glm::vec3 local_dir = local_dir_raw / dir_length;

		/* 先跟包围盒求交，把步进限制在有地形的那一段 */
		const glm::vec3 box_min(-m_Extent.x, 0.0f, -m_Extent.y);
		const glm::vec3 box_max(m_Extent.x, std::max(m_HeightScale, 1e-3f), m_Extent.y);
		float t_enter = 0.0f;
		float t_exit = 1.0e9f;
		for (int axis = 0; axis < 3; ++axis)
		{
			if (std::fabs(local_dir[axis]) < 1e-6f)
			{
				if (local_origin[axis] < box_min[axis] || local_origin[axis] > box_max[axis])
					return false;
				continue;
			}

			const float inverse = 1.0f / local_dir[axis];
			float near_t = (box_min[axis] - local_origin[axis]) * inverse;
			float far_t = (box_max[axis] - local_origin[axis]) * inverse;
			if (near_t > far_t)
				std::swap(near_t, far_t);
			t_enter = std::max(t_enter, near_t);
			t_exit = std::min(t_exit, far_t);
			if (t_enter > t_exit)
				return false;
		}

		/* 步进找第一次穿到地表以下，再在跨过的那一段里二分。
		 * 离得越远步子越大：近处要贴住细节，远处只求命中 */
		const float texel = (std::max(m_Extent.x, m_Extent.y) * 2.0f)
			/ static_cast<float>(std::max(m_Resolution, 2u) - 1u);
		const auto height_at = [this](const glm::vec3& point, float& out_height)
		{
			return TerrainSampling::SampleHeight(m_Heights.data(), m_Resolution, m_Extent,
				m_HeightScale, glm::vec2(point.x, point.z), out_height);
		};

		/* 起点就在地下（相机贴地 / 站在山坡里）：直接算命中 */
		{
			const glm::vec3 point = local_origin + local_dir * t_enter;
			float height = 0.0f;
			if (height_at(point, height) && point.y <= height)
			{
				const glm::vec3 world_hit = glm::vec3(m_World * glm::vec4(point, 1.0f));
				out_local = glm::vec2(point.x, point.z);
				out_distance = glm::length(world_hit - origin);
				return true;
			}
		}

		float previous_t = t_enter;
		bool has_previous = false;
		for (float t = t_enter; t <= t_exit;)
		{
			const glm::vec3 point = local_origin + local_dir * t;
			float height = 0.0f;
			if (!height_at(point, height))
			{
				t += texel * 2.0f;
				continue;
			}

			const float delta = point.y - height;
			if (has_previous && delta <= 0.0f)
			{
				/* 上一段还在地表之上、这一段到地下了：二分到交点 */
				float low = previous_t;
				float high = t;
				for (int step = 0; step < 16; ++step)
				{
					const float mid = (low + high) * 0.5f;
					const glm::vec3 mid_point = local_origin + local_dir * mid;
					float mid_height = 0.0f;
					const bool inside = height_at(mid_point, mid_height);
					if (!inside || mid_point.y - mid_height > 0.0f)
						low = mid;
					else
						high = mid;
				}

				const glm::vec3 hit = local_origin + local_dir * high;
				const glm::vec3 world_hit = glm::vec3(m_World * glm::vec4(hit, 1.0f));
				out_local = glm::vec2(hit.x, hit.z);
				out_distance = glm::length(world_hit - origin);
				return true;
			}

			previous_t = t;
			has_previous = true;
			t += std::max(texel * 2.0f, t * 0.004f);
		}

		return false;
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

		if (m_NodeTable.empty())
			return;

		/* 根节点盖住整块地形（局部空间），从它往下一层层细分 */
		CollectNodes(&frustum, eye, 0.0f, 0, glm::uvec2(0), out);
		BalancePatches(out);
		MarkMorphPatches(out);
	}

	void TerrainField::BuildPatchesAround(const glm::vec3& center, float radius,
		std::vector<TerrainPatch>& out) const
	{
		if (m_HeightMap == nullptr || radius <= 0.0f || m_NodeTable.empty())
			return;

		CollectNodes(nullptr, center, radius, 0, glm::uvec2(0), out);
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
					TerrainNode node;
					node.Rect = glm::vec4(center.x, center.y, node_size, static_cast<float>(level));
					m_NodeTable.push_back(node);
				}
			}

			m_MaxNodeLevel = level;
			if (node_size <= min_node * 1.01f)
				break;
		}

		UpdateNodeHeights(glm::uvec2(0), glm::uvec2(m_Resolution - 1));

		/* 表换了，显存那份下一帧重建 */
		m_NodeStorage = nullptr;
		m_NodeUploadedRevision = 0;
	}

	uint32_t TerrainField::NodeIndex(uint32_t level, const glm::uvec2& block)
	{
		const uint32_t side = 1u << level;
		const uint32_t offset = ((1u << (2u * level)) - 1u) / 3u;
		return offset + block.y * side + block.x;
	}

	void TerrainField::UpdateNodeHeights(const glm::uvec2& texel_min, const glm::uvec2& texel_max)
	{
		if (m_NodeTable.empty() || m_Heights.empty() || m_Resolution < 2)
			return;

		const float root_size = m_NodeTable[0].Rect.z;
		const float half_root = root_size * 0.5f;
		const float max_index = static_cast<float>(m_Resolution - 1);
		const glm::vec2 texel_size = m_Extent * 2.0f / max_index;

		/* 节点矩形 → 纹素范围：双线性会碰到外侧一格，取 floor / ceil */
		const auto scan = [&](const glm::vec4& rect)
		{
			const glm::vec2 half(rect.z * 0.5f);
			const glm::vec2 uv0 = glm::clamp((glm::vec2(rect) - half) / (m_Extent * 2.0f) + 0.5f, 0.0f, 1.0f);
			const glm::vec2 uv1 = glm::clamp((glm::vec2(rect) + half) / (m_Extent * 2.0f) + 0.5f, 0.0f, 1.0f);
			const glm::uvec2 t0(glm::floor(uv0 * max_index));
			const glm::uvec2 t1(glm::ceil(uv1 * max_index));

			float lo = std::numeric_limits<float>::max();
			float hi = -std::numeric_limits<float>::max();
			for (uint32_t z = t0.y; z <= t1.y; ++z)
			{
				for (uint32_t x = t0.x; x <= t1.x; ++x)
				{
					const float h = m_Heights[static_cast<size_t>(z) * m_Resolution + x];
					lo = std::min(lo, h);
					hi = std::max(hi, h);
				}
			}
			return glm::vec4(lo * m_HeightScale, hi * m_HeightScale, 0.0f, 0.0f);
		};

		/* 改动矩形落到最细一层的块范围，往外各扩一块兜住边上的取样 */
		const int side = 1 << m_MaxNodeLevel;
		const float node_size = root_size / static_cast<float>(side);
		const auto to_block = [&](const glm::uvec2& texel, int margin)
		{
			const glm::vec2 local = -m_Extent + glm::vec2(texel) * texel_size;
			const glm::ivec2 block(glm::floor((local + half_root) / node_size));
			return glm::clamp(block + margin, glm::ivec2(0), glm::ivec2(side - 1));
		};
		glm::ivec2 block_min = to_block(texel_min, -1);
		glm::ivec2 block_max = to_block(texel_max, 1);

		for (int z = block_min.y; z <= block_max.y; ++z)
		{
			for (int x = block_min.x; x <= block_max.x; ++x)
			{
				TerrainNode& node = m_NodeTable[NodeIndex(m_MaxNodeLevel, glm::uvec2(x, z))];
				node.Heights = scan(node.Rect);
			}
		}

		/* 往上逐层合并：父节点的范围是四个子节点的并 */
		for (uint32_t level = m_MaxNodeLevel; level > 0; --level)
		{
			block_min >>= 1;
			block_max >>= 1;
			for (int z = block_min.y; z <= block_max.y; ++z)
			{
				for (int x = block_min.x; x <= block_max.x; ++x)
				{
					glm::vec2 range(std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());
					for (int child = 0; child < 4; ++child)
					{
						const glm::uvec2 child_block(x * 2 + (child & 1), z * 2 + (child >> 1));
						const glm::vec4& heights = m_NodeTable[NodeIndex(level, child_block)].Heights;
						range = glm::vec2(std::min(range.x, heights.x), std::max(range.y, heights.y));
					}
					m_NodeTable[NodeIndex(level - 1, glm::uvec2(x, z))].Heights = glm::vec4(range, 0.0f, 0.0f);
				}
			}
		}

		++m_NodeTableRevision;
	}

	void TerrainField::GetNodeWorldBounds(const TerrainNode& node, glm::vec3& out_center,
		glm::vec3& out_extent) const
	{
		const glm::vec3 local_center(node.Rect.x, (node.Heights.x + node.Heights.y) * 0.5f, node.Rect.y);
		const glm::vec3 local_extent(node.Rect.z * 0.5f,
			std::max((node.Heights.y - node.Heights.x) * 0.5f, 0.01f), node.Rect.z * 0.5f);

		/* 世界半范围 = 各轴列向量取绝对值按局部半范围加权 */
		out_center = glm::vec3(m_World * glm::vec4(local_center, 1.0f));
		out_extent = glm::abs(glm::vec3(m_World[0])) * local_extent.x
			+ glm::abs(glm::vec3(m_World[1])) * local_extent.y
			+ glm::abs(glm::vec3(m_World[2])) * local_extent.z;
	}

	TerrainField::SelectionSlot* TerrainField::FindSelection(const RenderView* view)
	{
		for (SelectionSlot& slot : m_Selections)
		{
			if (slot.View == view)
				return &slot;
		}

		/* 没见过这个视图：先用空槽，再顶掉帧号最小的那一份（没选成功的帧号是最大值，也算空） */
		const auto age = [](const SelectionSlot& slot)
		{
			const bool unused = slot.View == nullptr || slot.Frame == std::numeric_limits<uint32_t>::max();
			return unused ? 0ull : static_cast<uint64_t>(slot.Frame) + 1ull;
		};
		SelectionSlot* oldest = &m_Selections[0];
		for (SelectionSlot& slot : m_Selections)
		{
			if (age(slot) < age(*oldest))
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

		const uint32_t node_bytes = static_cast<uint32_t>(m_NodeTable.size() * sizeof(TerrainNode));
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
			if (slot->Storage == nullptr || slot->ViewBuffer == nullptr)
			{
				slot->Storage = nullptr;
				slot->ViewBuffer = nullptr;
				return false;
			}
		}

		SharedPtr<DeviceStorageBuffer>& args_buffer = m_ArgsRing[m_ArgsCursor];
		if (args_buffer == nullptr)
			args_buffer = DeviceStorageBuffer::Create("TerrainSelectArgs", 5u * sizeof(uint32_t));
		if (args_buffer == nullptr)
			return false;
		m_ArgsCursor = (m_ArgsCursor + 1) % kArgsRing;
		slot->Args = args_buffer;

		const uint32_t args[5] = { kPatchIndexCount, 0u, 0u, 0u, 0u };
		slot->Args->SetData(args, sizeof(args));

		TerrainSelectParams params;
		params.Model = m_World;
		for (int index = 0; index < 6; ++index)
			params.Planes[index] = planes[index];

		const float world_scale = std::max(m_WorldScale.x, std::max(m_WorldScale.y, m_WorldScale.z));
		params.Eye = glm::vec4(eye, static_cast<float>(m_NodeTable.size()));
		params.Counts = glm::vec4(m_NodeTable[0].Rect.z, m_NodeTable.back().Rect.z, m_LodRatio, world_scale);
		params.Extra = glm::vec4(m_Extent.x, static_cast<float>(m_MaxNodeLevel), static_cast<float>(kMaxPatches),
			m_Extent.y);

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
		uint32_t level, const glm::uvec2& block, std::vector<TerrainPatch>& out) const
	{
		if (out.size() >= kMaxPatches)
			return;

		/* 根节点按长边取正方形：落在实际矩形外的节点不出，否则会画出一片拉伸的边缘 */
		const TerrainNode& node = m_NodeTable[NodeIndex(level, block)];
		if (std::fabs(node.Rect.x) - node.Rect.z * 0.5f >= m_Extent.x
			|| std::fabs(node.Rect.y) - node.Rect.z * 0.5f >= m_Extent.y)
			return;

		/* 剔除盒按节点自己的高度范围：只按 xz 或整块起伏算，山顶会被切掉或剔不干净 */
		glm::vec3 world_center(0.0f);
		glm::vec3 world_extent(0.0f);
		GetNodeWorldBounds(node, world_center, world_extent);
		if (frustum != nullptr && frustum->IsOutside(world_center, world_extent))
			return;

		/* 球范围（探针捕获）：盒子的半对角线算进去，边角节点的叶子可能还伸进范围里 */
		const float distance = glm::length(eye - world_center);
		if (max_distance > 0.0f && distance - glm::length(world_extent) > max_distance)
			return;

		/* LOD 半径是局部边长乘最大缩放：放大后的地形在同样的世界距离下更细 */
		const float max_scale = std::max(m_WorldScale.x, std::max(m_WorldScale.y, m_WorldScale.z));
		const float local_size = node.Rect.z;
		if (level < m_MaxNodeLevel && distance < local_size * max_scale * m_LodRatio)
		{
			for (uint32_t child = 0; child < 4; ++child)
			{
				CollectNodes(frustum, eye, max_distance, level + 1,
					block * 2u + glm::uvec2(child & 1u, child >> 1u), out);
			}
			return;
		}

		TerrainPatch patch;
		patch.Info = glm::vec4(node.Rect.x - local_size * 0.5f, 0.0f, node.Rect.y - local_size * 0.5f, local_size);
		/* 采样步长：节点边长除以 32 段 */
		patch.Lod = glm::vec4(static_cast<float>(level), local_size / static_cast<float>(kPatchQuads), 0.0f, 0.0f);
		out.push_back(patch);
	}

	namespace
	{
		/* 叶子查找表：patch 都是四叉树节点，按（级数, 块号）建索引，
		 * 「某点被哪块叶子盖着」从最细一层往上按块号查，不用两两比对 */
		struct PatchLookup
		{
			float Root{ 0.0f };
			int MaxLevel{ 0 };
			std::unordered_map<uint64_t, size_t> Index;

			static uint64_t Key(int level, const glm::ivec2& block)
			{
				return (static_cast<uint64_t>(level) << 48) | (static_cast<uint64_t>(static_cast<uint32_t>(block.x)) << 24)
					| static_cast<uint64_t>(static_cast<uint32_t>(block.y));
			}

			void Build(const std::vector<TerrainPatch>& patches)
			{
				Index.clear();
				MaxLevel = 0;
				if (patches.empty())
					return;

				const int first_level = static_cast<int>(patches[0].Lod.x + 0.5f);
				Root = patches[0].Info.w * static_cast<float>(1 << first_level);
				for (size_t index = 0; index < patches.size(); ++index)
				{
					const TerrainPatch& patch = patches[index];
					const int level = static_cast<int>(patch.Lod.x + 0.5f);
					const glm::vec2 corner(patch.Info.x, patch.Info.z);
					const glm::ivec2 block(glm::round((corner + Root * 0.5f) / patch.Info.w));
					Index[Key(level, block)] = index;
					MaxLevel = std::max(MaxLevel, level);
				}
			}

			/* 盖住这个点的叶子下标，点在地形外或那一片没画时返回 -1 */
			[[nodiscard]] int Find(const glm::vec2& point) const
			{
				const glm::vec2 offset = point + Root * 0.5f;
				if (offset.x < 0.0f || offset.y < 0.0f || offset.x >= Root || offset.y >= Root)
					return -1;

				for (int level = MaxLevel; level >= 0; --level)
				{
					const glm::ivec2 block(glm::floor(offset / (Root / static_cast<float>(1 << level))));
					const auto found = Index.find(Key(level, block));
					if (found != Index.end())
						return static_cast<int>(found->second);
				}
				return -1;
			}
		};

		/* 每条边往外探一格的探针，位序与 TerrainCommon.glsl 一致：1 = -X，2 = +X，4 = -Z，8 = +Z */
		void EdgeProbes(const TerrainPatch& patch, glm::vec2 (&out)[4])
		{
			constexpr float kProbe = 0.01f;
			const glm::vec2 min(patch.Info.x, patch.Info.z);
			const glm::vec2 max = min + patch.Info.w;
			const glm::vec2 mid = (min + max) * 0.5f;
			out[0] = { min.x - kProbe, mid.y };
			out[1] = { max.x + kProbe, mid.y };
			out[2] = { mid.x, min.y - kProbe };
			out[3] = { mid.x, max.y + kProbe };
		}
	}

	/* 四叉树没有 2:1 平衡：相邻叶子可能差两档以上，morph 只能对上差一档的情况。
	 * 这里做一遍平衡——邻居比自己粗两档以上就把邻居拆一层，循环到稳定为止 */
	void TerrainField::BalancePatches(std::vector<TerrainPatch>& patches)
	{
		constexpr int kMaxIterations = 8;
		PatchLookup lookup;
		for (int iteration = 0; iteration < kMaxIterations; ++iteration)
		{
			lookup.Build(patches);
			std::vector<uint8_t> victims(patches.size(), 0);
			bool any = false;
			for (const TerrainPatch& patch : patches)
			{
				glm::vec2 probes[4];
				EdgeProbes(patch, probes);
				for (const glm::vec2& probe : probes)
				{
					const int other = lookup.Find(probe);
					if (other >= 0 && patches[static_cast<size_t>(other)].Info.w > patch.Info.w * 2.01f)
					{
						victims[static_cast<size_t>(other)] = 1;
						any = true;
					}
				}
			}
			if (!any)
				break;

			/* 拆邻居而不是拆自己：细节分布保持不变 */
			std::vector<TerrainPatch> next;
			next.reserve(patches.size() + 16);
			for (size_t index = 0; index < patches.size(); ++index)
			{
				const TerrainPatch& patch = patches[index];
				if (victims[index] == 0)
				{
					next.push_back(patch);
					continue;
				}

				const float child = patch.Info.w * 0.5f;
				for (int quadrant = 0; quadrant < 4; ++quadrant)
				{
					TerrainPatch child_patch;
					child_patch.Info = glm::vec4(patch.Info.x + ((quadrant & 1) ? child : 0.0f), 0.0f,
						patch.Info.z + ((quadrant & 2) ? child : 0.0f), child);
					child_patch.Lod = glm::vec4(patch.Lod.x + 1.0f, child / static_cast<float>(kPatchQuads), 0.0f, 0.0f);
					next.push_back(child_patch);
				}
			}
			patches = std::move(next);
		}
	}

	/* 逐条边标记：哪一侧挨着更粗的邻居，那一条边就 morph 到父级表面。
	 * 同层邻居两边都不 morph（细表面本来就一模一样），只有细的一侧收敛 —— 两边都收
	 * 会收敛到不同精度的表面，交界反而裂成一道墙 */
	void TerrainField::MarkMorphPatches(std::vector<TerrainPatch>& patches)
	{
		PatchLookup lookup;
		lookup.Build(patches);
		for (TerrainPatch& patch : patches)
		{
			int mask = 0;
			if (patch.Lod.x > 0.5f)
			{
				glm::vec2 probes[4];
				EdgeProbes(patch, probes);
				for (int side = 0; side < 4; ++side)
				{
					const int other = lookup.Find(probes[side]);
					if (other >= 0 && patches[static_cast<size_t>(other)].Lod.x < patch.Lod.x)
						mask |= 1 << side;
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
