#pragma once
#include <cstdint>
#include <vector>

#include <RmlUi/Core/Types.h>

#include "Helios/Common/Common.h"

namespace Helios
{
	class DeviceTexture;

	namespace UI
	{
		/* 字形图集：把 RmlUi 生成的几张字体图集拼进一张纹理，纯色几何取左上角的白点。
		 * 拼成一张之后背景、边框与文字同纹理，回放时才能并成一条绘制命令。 */
		class UIGlyphAtlas final
		{
		public:
			static constexpr uint32_t kSize = 1024;

			/* 拼进图集的一块：内容左上角与尺寸（像素） */
			struct Region
			{
				uint32_t X{ 0 };
				uint32_t Y{ 0 };
				uint32_t Width{ 0 };
				uint32_t Height{ 0 };
			};

			/* 一份 source_width × source_height 的位图放进来；放不下返回 false */
			bool Add(Rml::Span<const Rml::byte> source, uint32_t source_width, uint32_t source_height,
				Region& out_region);
			/* 字体重新生成时旧块作废，槽位留给下一份用 */
			void Remove(const Region& region);

			[[nodiscard]] const SharedPtr<DeviceTexture>& GetTexture() const { return m_Texture; }
			/* 左上角那个白点的 UV：纯色几何直接取它 */
			[[nodiscard]] Rml::Vector2f GetWhiteUv() const;
			[[nodiscard]] uint64_t GetBytes() const;

		private:
			[[nodiscard]] bool EnsureReady();
			[[nodiscard]] bool Allocate(uint32_t width, uint32_t height, uint32_t& x, uint32_t& y);
			void Upload();

			SharedPtr<DeviceTexture> m_Texture{};
			std::vector<Rml::byte> m_Pixels{};
			/* 货架式排布：从 (1,1) 起往右排，排不下换一行；(0,0) 永远留白 */
			uint32_t m_ShelfX{ 1 };
			uint32_t m_ShelfY{ 1 };
			uint32_t m_ShelfHeight{ 0 };
			/* 释放过的块：按尺寸找回，不切分（图集重建次数很少，够用） */
			std::vector<Region> m_Free{};
		};
	}
}
