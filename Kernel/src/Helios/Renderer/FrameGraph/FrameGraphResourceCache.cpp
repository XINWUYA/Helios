#include "Pch.h"
#include "FrameGraphResourceCache.h"

namespace Helios
{
	/* 帧入口：推进帧号（槽位随之轮转），淘汰错过槽位的条目，复位占用标记 */
	void FrameGraphResourceCache::BeginFrame()
	{
		++m_FrameCounter;

		/* 淘汰：连续 kFrameCopyCount + 1 帧没用过就清掉。轮转下"隔帧才用"是常态（稳态副本的
		 * LastUsedFrame 恒为 当前帧号 - 2），两帧的阈值会把本该轮到的那份提前清掉；超过一个完整
		 * 轮转周期才算过期。 */
		auto sweep = [this](auto& cache)
		{
			for (auto iter = cache.begin(); iter != cache.end();)
			{
				if (iter->second.LastUsedFrame + kFrameCopyCount + 1 <= m_FrameCounter)
					iter = cache.erase(iter);
				else
				{
					iter->second.InUse = false;
					++iter;
				}
			}
		};
		sweep(m_TextureCache);
		sweep(m_FrameBufferCache);
	}

	/* 获取（或创建）一张纹理（当前槽位的副本） */
	SharedPtr<DeviceTexture> FrameGraphResourceCache::AcquireTexture(const std::string& name, const TextureDesc& desc)
	{
		const std::string key = MakeTextureKey(name, desc, CurrentSlot());

		const auto iter = m_TextureCache.find(key);
		if (iter != m_TextureCache.end())
		{
			auto& entry = iter->second;
			/* 同帧内已用过的条目（占用中，或释放后又请求）不允许再次复用：
			 * 同一帧里两张同名纹理若共用一份物理存储，后者会覆写前者尚未消费的内容 */
			if (entry.InUse || entry.LastUsedFrame == m_FrameCounter)
				return nullptr;

			entry.InUse = true;
			entry.LastUsedFrame = m_FrameCounter;
			++m_Stats.TextureReuses;
			return entry.Texture;
		}

		auto texture = DeviceTexture::Create(name, desc);
		if (texture)
		{
			m_TextureCache.insert({ key, TextureEntry{ texture, m_FrameCounter, true } });
			++m_Stats.TextureCreations;
		}
		return texture;
	}

	/* 归还纹理：仅当归还对象就是当前槽位条目时生效（同帧临时分配的对象不牵动缓存）。
	 * 注：归还要么发生在同一帧内（帧号未变），要么发生在下一帧 Reset 时（BeginFrame
	 * 尚未推进帧号）—— 两种情况算出的槽位都与 Acquire 时一致。 */
	void FrameGraphResourceCache::ReleaseTexture(const std::string& name, const TextureDesc& desc,
		const SharedPtr<DeviceTexture>& texture)
	{
		if (!texture)
			return;

		const auto iter = m_TextureCache.find(MakeTextureKey(name, desc, CurrentSlot()));
		if (iter != m_TextureCache.end() && iter->second.Texture == texture)
			iter->second.InUse = false;
	}

	/* 获取（或创建）一个 FrameBuffer（当前槽位的副本） */
	SharedPtr<DeviceFrameBuffer> FrameGraphResourceCache::AcquireFrameBuffer(const std::string& name, const FrameBufferDesc& desc)
	{
		const std::string key = MakeFrameBufferKey(name, CurrentSlot());

		const auto iter = m_FrameBufferCache.find(key);
		if (iter != m_FrameBufferCache.end())
		{
			auto& entry = iter->second;
			/* 同槽位内名称相同且描述一致 → 复用；描述变化 → 换新（覆盖旧条目）； */
			if (!entry.InUse && IsFrameBufferDescCompatible(entry.Desc, desc))
			{
				entry.InUse = true;
				entry.LastUsedFrame = m_FrameCounter;
				++m_Stats.FrameBufferReuses;
				return entry.FrameBuffer;
			}

			if (entry.InUse)
			{
				/* 同帧重名：临时分配（与优化前行为一致），不进缓存 */
				auto transient = DeviceFrameBuffer::Create(name, desc);
				if (transient)
					++m_Stats.FrameBufferCreations;
				return transient;
			}
		}

		auto frame_buffer = DeviceFrameBuffer::Create(name, desc);
		if (frame_buffer)
		{
			m_FrameBufferCache.insert_or_assign(key, FrameBufferEntry{ desc, frame_buffer, m_FrameCounter, true });
			++m_Stats.FrameBufferCreations;
		}
		return frame_buffer;
	}

	/* 纹理键：名称 + 全量描述字段 + 槽位 */
	std::string FrameGraphResourceCache::MakeTextureKey(const std::string& name, const TextureDesc& desc, uint32_t slot)
	{
		std::string key;
		key.reserve(name.size() + 72);
		key.append(name);
		key.push_back('|');
		key.append(std::to_string(desc.Width)).push_back('x');
		key.append(std::to_string(desc.Height)).push_back('x');
		key.append(std::to_string(desc.Depth)).push_back('x');
		key.append(std::to_string(desc.MipLevels)).push_back('x');
		key.append(std::to_string(desc.Samples)).push_back('|');
		key.append(std::to_string(static_cast<uint32_t>(desc.Format))).push_back('|');
		key.append(std::to_string(static_cast<uint32_t>(desc.SamplerType))).push_back('|');
		key.append(std::to_string(static_cast<uint32_t>(desc.Usage))).push_back('|');
		key.append(std::to_string(slot));
		return key;
	}

	/* FrameBuffer 键：名称 + 槽位 */
	std::string FrameGraphResourceCache::MakeFrameBufferKey(const std::string& name, uint32_t slot)
	{
		std::string key;
		key.reserve(name.size() + 4);
		key.append(name);
		key.push_back('|');
		key.append(std::to_string(slot));
		return key;
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
