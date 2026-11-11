#include "Pch.h"
#include "UIGlyphAtlas.h"

#include "Helios/VirtualDevice/DeviceTexture.h"

namespace Helios
{
	namespace UI
	{
		bool UIGlyphAtlas::EnsureReady()
		{
			if (m_Texture != nullptr)
				return true;

			TextureDesc desc{};
			desc.Width = kSize;
			desc.Height = kSize;
			desc.Format = TextureFormat::RGBA8;
			desc.Usage = TextureUsage::Sampleable;
			m_Texture = DeviceTexture::Create("UIGlyphAtlas", desc);
			if (!m_Texture)
				return false;

			/* 整张先刷白：(0,0) 留给纯色几何，其余没用到的地方也不会画错 */
			m_Pixels.assign(static_cast<size_t>(kSize) * kSize * 4u, 255u);
			Upload();
			return true;
		}

		bool UIGlyphAtlas::Add(Rml::Span<const Rml::byte> source, uint32_t source_width,
			uint32_t source_height, Region& out_region)
		{
			if (source_width == 0 || source_height == 0 || !EnsureReady())
				return false;

			uint32_t x = 0;
			uint32_t y = 0;
			if (!Allocate(source_width, source_height, x, y))
				return false;

			for (uint32_t row = 0; row < source_height; ++row)
			{
				const Rml::byte* source_row = source.data() + static_cast<size_t>(row) * source_width * 4u;
				Rml::byte* target_row = m_Pixels.data()
					+ (static_cast<size_t>(y + row) * kSize + x) * 4u;
				std::memcpy(target_row, source_row, static_cast<size_t>(source_width) * 4u);
			}
			Upload();

			out_region = Region{ x, y, source_width, source_height };
			return true;
		}

		void UIGlyphAtlas::Remove(const Region& region)
		{
			if (region.Width == 0 || region.Height == 0)
				return;
			m_Free.push_back(region);
		}

		Rml::Vector2f UIGlyphAtlas::GetWhiteUv() const
		{
			return Rml::Vector2f(0.5f / static_cast<float>(kSize), 0.5f / static_cast<float>(kSize));
		}

		uint64_t UIGlyphAtlas::GetBytes() const
		{
			return m_Texture != nullptr ? static_cast<uint64_t>(kSize) * kSize * 4u : 0;
		}

		bool UIGlyphAtlas::Allocate(uint32_t width, uint32_t height, uint32_t& x, uint32_t& y)
		{
			/* 先找释放过的块：字体重新生成时走这里 */
			for (size_t index = 0; index < m_Free.size(); ++index)
			{
				const Region& region = m_Free[index];
				if (region.Width >= width && region.Height >= height)
				{
					x = region.X;
					y = region.Y;
					m_Free.erase(m_Free.begin() + static_cast<std::ptrdiff_t>(index));
					return true;
				}
			}

			if (m_ShelfX + width > kSize)
			{
				m_ShelfY += m_ShelfHeight;
				m_ShelfX = 1;
				m_ShelfHeight = 0;
			}
			if (m_ShelfY + height > kSize)
				return false;

			x = m_ShelfX;
			y = m_ShelfY;
			m_ShelfX += width;
			m_ShelfHeight = std::max(m_ShelfHeight, height);
			return true;
		}

		void UIGlyphAtlas::Upload()
		{
			PixelDesc desc{};
			desc.Format = PixelFormat::RGBA;
			desc.Type = PixelType::UnsignedByte;
			m_Texture->SetData(m_Pixels.data(), desc);
		}
	}
}
