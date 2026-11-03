#include "Pch.h"
#include "TerrainVirtualTexture.h"

#include <algorithm>
#include <cmath>

#include "TerrainField.h"
#include "Helios/Renderer/Renderer.h"
#include "Helios/VirtualDevice/DeviceTexture.h"

namespace Helios
{
	namespace
	{
		bool s_Enabled = false;
		uint32_t s_BakeBudget = 4;
		uint32_t s_PageTexels = 256;
	}

	void TerrainVirtualTexture::SetEnabled(bool enabled)
	{
		s_Enabled = enabled;
	}

	bool TerrainVirtualTexture::IsEnabled()
	{
		return s_Enabled;
	}

	void TerrainVirtualTexture::SetBakeBudget(uint32_t pages_per_frame)
	{
		s_BakeBudget = std::clamp(pages_per_frame, 0u, GetPoolLayers());
	}

	void TerrainVirtualTexture::SetPageTexels(uint32_t texels)
	{
		/* 只收 2 的幂档位：页池与采样都按页边长算，别的值没有意义 */
		if (texels != 128 && texels != 256 && texels != 512)
			return;
		s_PageTexels = texels;
	}

	uint32_t TerrainVirtualTexture::GetPageTexels()
	{
		return s_PageTexels;
	}

	uint32_t TerrainVirtualTexture::GetPoolLayers()
	{
		const uint32_t page_bytes = s_PageTexels * s_PageTexels * 4u;
		return std::clamp(kPoolBudgetBytes / std::max(page_bytes, 1u), 16u, 256u);
	}

	uint32_t TerrainVirtualTexture::GetBakeBudget()
	{
		return s_BakeBudget;
	}

	const SharedPtr<DeviceTexture>& TerrainVirtualTexture::GetFallbackAtlas()
	{
		static SharedPtr<DeviceTexture> s_Fallback;
		if (s_Fallback == nullptr && Renderer::GetRenderAPI() != nullptr)
		{
			TextureDesc desc;
			desc.Width = 1;
			desc.Height = 1;
			desc.Depth = 1;
			desc.MipLevels = 1;
			desc.Format = TextureFormat::RGBA8;
			desc.SamplerType = SamplerType::Sampler2DArray;
			desc.Usage = TextureUsage::Sampleable;
			s_Fallback = DeviceTexture::Create("TerrainVTFallbackAtlas", desc);
		}
		return s_Fallback;
	}

	bool TerrainVirtualTexture::EnsureResources()
	{
		if (!s_Enabled || Renderer::GetRenderAPI() == nullptr)
			return false;

		/* 档位换了：页池、页表、层映射一起重建，旧页一律作废 */
		if (m_Pool != nullptr && (m_PoolTexels != s_PageTexels || m_PoolLayers != GetPoolLayers()))
		{
			m_Pool = nullptr;
			m_BakeFrameBuffer = nullptr;
			m_PageTable = nullptr;
			m_Table.clear();
			m_LayerPages.clear();
			m_Layers.clear();
			m_Grid = glm::vec2(0.0f);
		}
		if (m_Pool != nullptr)
			return true;

		TextureDesc desc;
		desc.Width = s_PageTexels;
		desc.Height = s_PageTexels;
		desc.Depth = GetPoolLayers();
		desc.MipLevels = 1;
		desc.Format = TextureFormat::RGBA8;
		desc.SamplerType = SamplerType::Sampler2DArray;
		desc.Usage = TextureUsage::ColorAttachment | TextureUsage::Sampleable;
		m_Pool = DeviceTexture::Create("TerrainVTPool", desc);
		m_PoolTexels = s_PageTexels;
		m_PoolLayers = GetPoolLayers();
		return m_Pool != nullptr;
	}

	void TerrainVirtualTexture::ClearTable()
	{
		if (m_Table.empty())
			return;

		std::fill(m_Table.begin(), m_Table.end(), 0.0f);
		if (m_PageTable != nullptr)
		{
			PixelDesc pixel_desc;
			pixel_desc.Format = PixelFormat::R;
			pixel_desc.Type = PixelType::Float;
			m_PageTable->SetData(m_Table.data(), pixel_desc);
		}
	}

	void TerrainVirtualTexture::Reset()
	{
		ClearTable();
		std::fill(m_LayerPages.begin(), m_LayerPages.end(), glm::uvec2(0xFFFFFFFFu));
		std::fill(m_Layers.begin(), m_Layers.end(), LayerState{});
		m_Pending.clear();
	}

	const SharedPtr<DeviceFrameBuffer>& TerrainVirtualTexture::GetBakeFrameBuffer()
	{
		if (m_BakeFrameBuffer == nullptr && m_Pool != nullptr)
		{
			RenderBufferInfo color_info;
			color_info.RenderTarget = m_Pool;

			FrameBufferDesc desc;
			desc.Samples = 1;
			desc.Usage = RenderBufferUsage::ColorDefault;
			desc.ViewportRegion = ViewportRegion{ 0, 0, s_PageTexels, s_PageTexels };
			desc.ColorRenderBuffers.push_back(color_info);
			m_BakeFrameBuffer = DeviceFrameBuffer::Create("TerrainVTPageFrameBuffer", desc);
		}
		return m_BakeFrameBuffer;
	}

	bool TerrainVirtualTexture::BeginFrame(const TerrainField& field, glm::vec2& out_min_corner,
		uint32_t& out_grid_w, uint32_t& out_grid_h)
	{
		m_Pending.clear();
		if (!s_Enabled || field.IsEmpty())
			return false;
		if (!EnsureResources())
			return false;

		/* 页按世界空间切：旋转后取整个地形的轴对齐包围盒 */
		const glm::vec2 world_half = field.GetWorldHalfExtent();
		const glm::vec2 grid = glm::clamp(glm::ceil(world_half * 2.0f / kPageWorldSize),
			glm::vec2(1.0f), glm::vec2(static_cast<float>(kMaxGrid)));
		const glm::vec2 grid_size(static_cast<float>(static_cast<uint32_t>(grid.x)),
			static_cast<float>(static_cast<uint32_t>(grid.y)));

		if (m_PageTable == nullptr || grid_size != m_Grid)
		{
			m_Grid = grid_size;
			m_Table.assign(static_cast<size_t>(grid_size.x) * static_cast<size_t>(grid_size.y), 0.0f);
			m_LayerPages.assign(GetPoolLayers(), glm::uvec2(0xFFFFFFFFu));
			m_Layers.assign(GetPoolLayers(), LayerState{});

			TextureDesc desc;
			desc.Width = static_cast<uint32_t>(grid_size.x);
			desc.Height = static_cast<uint32_t>(grid_size.y);
			desc.MipLevels = 1;
			desc.Format = TextureFormat::R32F;
			desc.SamplerType = SamplerType::Sampler2D;
			desc.Usage = TextureUsage::Sampleable;
			m_PageTable = DeviceTexture::Create("TerrainVTPageTable", desc);
			ClearTable();
		}

		if (m_PageTable == nullptr)
			return false;

		out_min_corner = glm::vec2(field.GetOrigin().x, field.GetOrigin().z) - world_half;
		out_grid_w = static_cast<uint32_t>(grid_size.x);
		out_grid_h = static_cast<uint32_t>(grid_size.y);
		return true;
	}

	void TerrainVirtualTexture::QueueMissing(const std::vector<uint8_t>& needed,
		const glm::vec2& min_corner, uint32_t grid_w, uint32_t grid_h, const glm::vec3& eye, uint32_t frame)
	{
		struct Candidate
		{
			uint32_t X{ 0 };
			uint32_t Z{ 0 };
			float Distance{ 0.0f };
		};
		const uint32_t stamp = frame + 1u;
		const glm::vec2 eye_xz(eye.x, eye.z);
		const auto page_distance = [&](uint32_t x, uint32_t z)
		{
			return glm::distance(eye_xz, min_corner + (glm::vec2(x, z) + 0.5f) * kPageWorldSize);
		};

		/* 层的优先级 = 近两帧里离最近那个视图的距离：一帧多个视图共用页池，
		 * 按它排谁先让位，不会因为谁先画就把别人的近处页挤掉 */
		const auto touch = [&](uint32_t layer, float distance)
		{
			LayerState& state = m_Layers[layer];
			if (state.Used < stamp)
			{
				state.Previous = state.Used + 1u == stamp ? state.Current : kFarDistance;
				state.Current = distance;
				state.Used = stamp;
			}
			state.Current = std::min(state.Current, distance);
		};
		const auto priority = [&](const LayerState& state)
		{
			if (state.Used + 1u < stamp)
				return kFarDistance;
			return state.Used == stamp ? std::min(state.Current, state.Previous) : state.Current;
		};

		std::vector<Candidate> candidates;
		candidates.reserve(64);
		for (uint32_t z = 0; z < grid_h; ++z)
		{
			for (uint32_t x = 0; x < grid_w; ++x)
			{
				const size_t index = static_cast<size_t>(z) * grid_w + x;
				if (needed[index] == 0)
					continue;
				if (m_Table[index] != 0.0f)
					touch(static_cast<uint32_t>(m_Table[index]) - 1u, page_distance(x, z));
				else
					candidates.push_back(Candidate{ x, z, page_distance(x, z) });
			}
		}
		if (candidates.empty() || s_BakeBudget == 0)
			return;

		std::sort(candidates.begin(), candidates.end(),
			[](const Candidate& left, const Candidate& right) { return left.Distance < right.Distance; });

		/* 挑层：空层先用，再换优先级最低（离所有视图都最远、或两帧没人用）的那层；
		 * 它都比缺页近就停手 —— 页池装不下视野时近处先占满，远处退回底图或直接算 */
		const uint32_t layers = static_cast<uint32_t>(m_Layers.size());
		const auto pick_layer = [&](float distance)
		{
			uint32_t best = layers;
			float worst = distance;
			for (uint32_t layer = 0; layer < layers; ++layer)
			{
				if (m_Layers[layer].Used == 0)
					return layer;
				const float layer_priority = priority(m_Layers[layer]);
				if (layer_priority > worst)
				{
					worst = layer_priority;
					best = layer;
				}
			}
			return best;
		};

		for (const Candidate& candidate : candidates)
		{
			if (m_Pending.size() >= s_BakeBudget)
				break;

			const uint32_t layer = pick_layer(candidate.Distance);
			if (layer >= layers)
				break;

			/* 层被别的页占着就先清掉它：那一页下一帧会当成缺页重烘 */
			const glm::uvec2 previous = m_LayerPages[layer];
			if (previous.x != 0xFFFFFFFFu)
				m_Table[static_cast<size_t>(previous.y) * grid_w + previous.x] = 0.0f;
			m_LayerPages[layer] = glm::uvec2(0xFFFFFFFFu);
			m_Layers[layer] = LayerState{};
			touch(layer, candidate.Distance);

			m_Pending.push_back(glm::uvec3(candidate.X, candidate.Z, layer));
		}
	}

	void TerrainVirtualTexture::Prepare(const TerrainField& field, const TerrainPatch* patches,
		size_t patch_count, const glm::vec3& eye, uint32_t frame)
	{
		glm::vec2 min_corner(0.0f);
		uint32_t grid_w = 0;
		uint32_t grid_h = 0;
		if (!BeginFrame(field, min_corner, grid_w, grid_h))
			return;
		if (patches == nullptr || patch_count == 0)
			return;

		/* 本帧 patch 覆盖到的页：只需要标记，不重复入队 */
		std::vector<uint8_t> needed(m_Table.size(), 0);
		for (size_t patch_index = 0; patch_index < patch_count; ++patch_index)
		{
			const TerrainPatch& patch = patches[patch_index];

			/* patch 是局部矩形：四角变换到世界后取包围盒，页按世界切 */
			const glm::vec2 local_min(patch.Info.x, patch.Info.z);
			const glm::vec2 local_max = local_min + patch.Info.w;
			glm::vec2 world_min(std::numeric_limits<float>::max());
			glm::vec2 world_max(-std::numeric_limits<float>::max());
			for (int corner = 0; corner < 4; ++corner)
			{
				const glm::vec2 local((corner & 1) ? local_max.x : local_min.x,
					(corner & 2) ? local_max.y : local_min.y);
				const glm::vec3 world = field.ToWorld(local, 0.0f);
				world_min = glm::min(world_min, glm::vec2(world.x, world.z));
				world_max = glm::max(world_max, glm::vec2(world.x, world.z));
			}

			const glm::vec2 page_min = (world_min - min_corner) / kPageWorldSize;
			const glm::vec2 page_max = (world_max - min_corner) / kPageWorldSize;
			const int x0 = std::clamp(static_cast<int>(std::floor(page_min.x)), 0, static_cast<int>(grid_w) - 1);
			const int x1 = std::clamp(static_cast<int>(std::floor(page_max.x - 1e-4f)), 0, static_cast<int>(grid_w) - 1);
			const int z0 = std::clamp(static_cast<int>(std::floor(page_min.y)), 0, static_cast<int>(grid_h) - 1);
			const int z1 = std::clamp(static_cast<int>(std::floor(page_max.y - 1e-4f)), 0, static_cast<int>(grid_h) - 1);
			for (int z = z0; z <= z1; ++z)
			{
				for (int x = x0; x <= x1; ++x)
					needed[static_cast<size_t>(z) * grid_w + static_cast<size_t>(x)] = 1;
			}
		}

		QueueMissing(needed, min_corner, grid_w, grid_h, eye, frame);
	}

	void TerrainVirtualTexture::PrepareFrustum(const TerrainField& field, const glm::vec4 (&planes)[6],
		const glm::vec3& eye, uint32_t frame)
	{
		glm::vec2 min_corner(0.0f);
		uint32_t grid_w = 0;
		uint32_t grid_h = 0;
		if (!BeginFrame(field, min_corner, grid_w, grid_h))
			return;

		/* patch 在 GPU 上选的时候拿不到覆盖列表：页盒与视锥求交，
		 * 选出来的就是屏幕上可能用得到的那一片 */
		const float height_min = field.GetMinWorldHeight();
		const float height_max = field.GetMaxWorldHeight();
		const glm::vec3 box_extent(kPageWorldSize * 0.5f,
			std::max((height_max - height_min) * 0.5f, 0.5f), kPageWorldSize * 0.5f);

		std::vector<uint8_t> needed(m_Table.size(), 0);
		for (uint32_t z = 0; z < grid_h; ++z)
		{
			for (uint32_t x = 0; x < grid_w; ++x)
			{
				const glm::vec2 center = min_corner + (glm::vec2(x, z) + 0.5f) * kPageWorldSize;
				const glm::vec3 box_center(center.x, (height_min + height_max) * 0.5f, center.y);

				bool outside = false;
				for (const glm::vec4& plane : planes)
				{
					const glm::vec3 normal(plane);
					if (glm::dot(normal, box_center) + plane.w + glm::dot(glm::abs(normal), box_extent) < 0.0f)
					{
						outside = true;
						break;
					}
				}

				if (!outside)
					needed[static_cast<size_t>(z) * grid_w + x] = 1;
			}
		}

		QueueMissing(needed, min_corner, grid_w, grid_h, eye, frame);
	}

	uint32_t TerrainVirtualTexture::GetResidentPages() const
	{
		uint32_t count = 0;
		for (const float layer : m_Table)
		{
			if (layer > 0.0f)
				++count;
		}
		return count;
	}

	void TerrainVirtualTexture::Commit()
	{
		if (m_Pending.empty() || m_PageTable == nullptr)
			return;

		const uint32_t grid_w = static_cast<uint32_t>(m_Grid.x);
		for (const glm::uvec3& page : m_Pending)
		{
			m_Table[static_cast<size_t>(page.y) * grid_w + page.x] = static_cast<float>(page.z + 1u);
			m_LayerPages[page.z] = glm::uvec2(page.x, page.y);
		}

		PixelDesc pixel_desc;
		pixel_desc.Format = PixelFormat::R;
		pixel_desc.Type = PixelType::Float;
		m_PageTable->SetData(m_Table.data(), pixel_desc);
		m_BakedPages += m_Pending.size();
		m_Pending.clear();
	}
}
