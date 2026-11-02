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
		s_BakeBudget = std::clamp(pages_per_frame, 0u, kPoolLayers);
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
		if (m_Pool != nullptr)
			return true;

		TextureDesc desc;
		desc.Width = kPageTexels;
		desc.Height = kPageTexels;
		desc.Depth = kPoolLayers;
		desc.MipLevels = 1;
		desc.Format = TextureFormat::RGBA8;
		desc.SamplerType = SamplerType::Sampler2DArray;
		desc.Usage = TextureUsage::ColorAttachment | TextureUsage::Sampleable;
		m_Pool = DeviceTexture::Create("TerrainVTPool", desc);
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
		m_NextLayer = 0;
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
			desc.ViewportRegion = ViewportRegion{ 0, 0, kPageTexels, kPageTexels };
			desc.ColorRenderBuffers.push_back(color_info);
			m_BakeFrameBuffer = DeviceFrameBuffer::Create("TerrainVTPageFrameBuffer", desc);
		}
		return m_BakeFrameBuffer;
	}

	void TerrainVirtualTexture::Prepare(const TerrainField& field, const TerrainPatch* patches,
		size_t patch_count, const glm::vec3& eye)
	{
		m_Pending.clear();
		if (!s_Enabled || field.IsEmpty() || patches == nullptr || patch_count == 0)
			return;
		if (!EnsureResources())
			return;

		const glm::vec2 extent = field.GetExtent();
		const glm::vec2 grid = glm::clamp(glm::ceil(extent * 2.0f / kPageWorldSize),
			glm::vec2(1.0f), glm::vec2(static_cast<float>(kMaxGrid)));
		const glm::vec2 grid_size(static_cast<float>(static_cast<uint32_t>(grid.x)),
			static_cast<float>(static_cast<uint32_t>(grid.y)));

		if (m_PageTable == nullptr || grid_size != m_Grid)
		{
			m_Grid = grid_size;
			m_Table.assign(static_cast<size_t>(grid_size.x) * static_cast<size_t>(grid_size.y), 0.0f);
			m_LayerPages.assign(kPoolLayers, glm::uvec2(0xFFFFFFFFu));
			m_NextLayer = 0;

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
			return;

		const uint32_t grid_w = static_cast<uint32_t>(grid_size.x);
		const uint32_t grid_h = static_cast<uint32_t>(grid_size.y);

		/* 本帧 patch 覆盖到的页：只需要标记，不重复入队 */
		static std::vector<uint8_t> needed;
		needed.assign(m_Table.size(), 0);

		const glm::vec2 min_corner = glm::vec2(field.GetOrigin().x, field.GetOrigin().z) - extent;
		for (size_t patch_index = 0; patch_index < patch_count; ++patch_index)
		{
			const TerrainPatch& patch = patches[patch_index];
			const glm::vec2 page_min = (glm::vec2(patch.Info.x, patch.Info.z) - min_corner) / kPageWorldSize;
			const glm::vec2 page_max = page_min + patch.Info.w / kPageWorldSize;
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

		struct Candidate
		{
			uint32_t X{ 0 };
			uint32_t Z{ 0 };
			float Distance{ 0.0f };
		};
		std::vector<Candidate> candidates;
		candidates.reserve(64);

		for (uint32_t z = 0; z < grid_h; ++z)
		{
			for (uint32_t x = 0; x < grid_w; ++x)
			{
				const size_t index = static_cast<size_t>(z) * grid_w + x;
				if (needed[index] == 0 || m_Table[index] != 0.0f)
					continue;

				const glm::vec2 center = min_corner + (glm::vec2(x, z) + 0.5f) * kPageWorldSize;
				candidates.push_back(Candidate{ x, z, glm::distance(glm::vec2(eye.x, eye.z), center) });
			}
		}

		const uint32_t budget = std::min(s_BakeBudget, static_cast<uint32_t>(candidates.size()));
		if (budget == 0)
			return;

		std::partial_sort(candidates.begin(), candidates.begin() + budget, candidates.end(),
			[](const Candidate& left, const Candidate& right) { return left.Distance < right.Distance; });

		for (uint32_t index = 0; index < budget; ++index)
		{
			const uint32_t layer = m_NextLayer % kPoolLayers;
			++m_NextLayer;

			/* 层被别的页占着就先清掉它：那一页下一帧会当成缺页重烘 */
			const glm::uvec2 previous = m_LayerPages[layer];
			if (previous.x != 0xFFFFFFFFu)
				m_Table[static_cast<size_t>(previous.y) * grid_w + previous.x] = 0.0f;

			m_Pending.push_back(glm::uvec3(candidates[index].X, candidates[index].Z, layer));
		}
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
		m_Pending.clear();
	}
}
