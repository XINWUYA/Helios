#include "Pch.h"
#include "FrameGraphResourceCache.h"

namespace Helios
{
	FrameGraphResourceCache::TextureKey::TextureKey(const TextureDesc& desc)
		: Width(desc.Width), Height(desc.Height), Depth(desc.Depth), MipLevels(desc.MipLevels),
		Samples(desc.Samples), Format(desc.Format), Sampler(desc.SamplerType), Usage(desc.Usage)
	{
	}

	bool FrameGraphResourceCache::TextureKey::operator==(const TextureKey& other) const
	{
		return Width == other.Width && Height == other.Height && Depth == other.Depth
			&& MipLevels == other.MipLevels && Samples == other.Samples && Format == other.Format
			&& Sampler == other.Sampler && Usage == other.Usage;
	}

	size_t FrameGraphResourceCache::TextureKeyHash::operator()(const TextureKey& key) const
	{
		uint64_t h = 0xCBF29CE484222325ULL;
		const auto mix = [&h](uint64_t value)
		{
			h ^= value + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
		};
		mix(key.Width);
		mix(key.Height);
		mix(key.Depth);
		mix((static_cast<uint64_t>(key.MipLevels) << 8) | key.Samples);
		mix(static_cast<uint64_t>(key.Format));
		mix(static_cast<uint64_t>(key.Sampler));
		mix(static_cast<uint64_t>(key.Usage));
		return static_cast<size_t>(h);
	}

	/* 帧入口：推进帧号（槽位随之轮转），淘汰错过槽位的条目，复位占用标记 */
	void FrameGraphResourceCache::BeginFrame()
	{
		++m_FrameCounter;

		/* 连续 kFrameCopyCount + 1 帧没用过才清掉：轮转下"隔帧才用"是常态，
		 * 稳态副本的 LastUsedFrame 恒为当前帧号 - 2，不能提前清 */
		const auto expired = [this](uint64_t last_used_frame)
		{
			return last_used_frame + kFrameCopyCount + 1 <= m_FrameCounter;
		};

		for (auto& pools : m_TexturePools)
		{
			for (auto pool_iter = pools.begin(); pool_iter != pools.end();)
			{
				auto& entries = pool_iter->second;
				entries.erase(std::remove_if(entries.begin(), entries.end(),
					[&expired](const TextureEntry& entry) { return expired(entry.LastUsedFrame); }), entries.end());
				for (auto& entry : entries)
					entry.InUse = false;

				pool_iter = entries.empty() ? pools.erase(pool_iter) : std::next(pool_iter);
			}
		}

		for (auto& frame_buffers : m_FrameBuffers)
		{
			for (auto iter = frame_buffers.begin(); iter != frame_buffers.end();)
			{
				if (expired(iter->second.LastUsedFrame))
				{
					iter = frame_buffers.erase(iter);
					continue;
				}
				iter->second.InUse = false;
				++iter;
			}
		}
	}

	/* 取当前槽位池里第一张空闲纹理（按插入顺序，同一张图每帧拿到的分配结果稳定） */
	SharedPtr<DeviceTexture> FrameGraphResourceCache::AcquireTexture(const std::string& name, const TextureDesc& desc)
	{
		auto& entries = m_TexturePools[CurrentSlot()][TextureKey(desc)];
		for (auto& entry : entries)
		{
			if (entry.InUse)
				continue;

			entry.InUse = true;
			entry.LastUsedFrame = m_FrameCounter;
			++m_Stats.TextureReuses;
			return entry.Texture;
		}

		auto texture = DeviceTexture::Create(name, desc);
		if (texture)
		{
			entries.push_back(TextureEntry{ texture, m_FrameCounter, true });
			++m_Stats.TextureCreations;
		}
		return texture;
	}

	/* 归还纹理。归还要么在同一帧内，要么在下一帧 Reset 时（BeginFrame 还没推进帧号），
	 * 两种情况算出的槽位都和取用时一致 */
	void FrameGraphResourceCache::ReleaseTexture(const TextureDesc& desc, const SharedPtr<DeviceTexture>& texture)
	{
		if (!texture)
			return;

		auto& pools = m_TexturePools[CurrentSlot()];
		const auto pool_iter = pools.find(TextureKey(desc));
		if (pool_iter == pools.end())
			return;

		for (auto& entry : pool_iter->second)
		{
			if (entry.Texture == texture)
			{
				entry.InUse = false;
				return;
			}
		}
	}

	/* 获取（或创建）一个 FrameBuffer（当前槽位的副本） */
	SharedPtr<DeviceFrameBuffer> FrameGraphResourceCache::AcquireFrameBuffer(const std::string& name, const FrameBufferDesc& desc)
	{
		auto& frame_buffers = m_FrameBuffers[CurrentSlot()];
		const auto iter = frame_buffers.find(name);
		if (iter != frame_buffers.end())
		{
			auto& entry = iter->second;
			/* 同槽位内名称相同且描述一致 → 复用；描述变化 → 换新（覆盖旧条目） */
			if (!entry.InUse && IsFrameBufferDescCompatible(entry.Desc, desc))
			{
				entry.InUse = true;
				entry.LastUsedFrame = m_FrameCounter;
				++m_Stats.FrameBufferReuses;
				return entry.FrameBuffer;
			}

			if (entry.InUse)
			{
				/* 同帧重名：临时分配，不进缓存 */
				auto transient = DeviceFrameBuffer::Create(name, desc);
				if (transient)
					++m_Stats.FrameBufferCreations;
				return transient;
			}
		}

		auto frame_buffer = DeviceFrameBuffer::Create(name, desc);
		if (frame_buffer)
		{
			frame_buffers.insert_or_assign(name, FrameBufferEntry{ desc, frame_buffer, m_FrameCounter, true });
			++m_Stats.FrameBufferCreations;
		}
		return frame_buffer;
	}

	/* FrameBuffer 描述逐字段比对：附件（纹理对象 + level/layer）、采样数、视口、
	 * 用途、清除值、叠加标志 —— 只要有任一变化就必须走新建。 */
	bool FrameGraphResourceCache::IsFrameBufferDescCompatible(const FrameBufferDesc& lhs, const FrameBufferDesc& rhs)
	{
		const auto same_render_buffer = [](const RenderBufferInfo& a, const RenderBufferInfo& b)
		{
			return a.RenderTarget == b.RenderTarget && a.Level == b.Level && a.Layer == b.Layer;
		};

		if (lhs.Samples != rhs.Samples
			|| lhs.Usage != rhs.Usage
			|| lhs.PreserveContent != rhs.PreserveContent
			|| !(lhs.ViewportRegion == rhs.ViewportRegion)
			|| lhs.ColorRenderBuffers.size() != rhs.ColorRenderBuffers.size()
			|| lhs.ColorClearValues.size() != rhs.ColorClearValues.size())
		{
			return false;
		}

		for (size_t i = 0; i < lhs.ColorRenderBuffers.size(); ++i)
		{
			if (!same_render_buffer(lhs.ColorRenderBuffers[i], rhs.ColorRenderBuffers[i]))
				return false;
		}

		/* 逐分量比对清除值（含"某附件是否显式指定"） */
		for (size_t i = 0; i < lhs.ColorClearValues.size(); ++i)
		{
			const auto& a = lhs.ColorClearValues[i];
			const auto& b = rhs.ColorClearValues[i];
			if (a.has_value() != b.has_value())
				return false;
			if (a.has_value()
				&& (a->x != b->x || a->y != b->y || a->z != b->z || a->w != b->w))
			{
				return false;
			}
		}

		return same_render_buffer(lhs.DepthRenderBuffer, rhs.DepthRenderBuffer)
			&& same_render_buffer(lhs.StencilRenderBuffer, rhs.StencilRenderBuffer);
	}
}
