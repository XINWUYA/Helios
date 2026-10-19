#pragma once
#include <cstdint>
#include <string>

namespace Helios
{
	/* 渲染层掩码：32 位对应 32 个层。对象可以同时属于多层，相机按可见层掩码取交集
	 * 决定画不画（见 RenderView::PrepareVisibleObjects）。 */
	using LayerMask = uint32_t;

	namespace Layers
	{
		constexpr uint32_t Count = 32;

		/* 内置层：前 6 个固定名字，覆盖常见的渲染分组；其余层按序号显示 Layer N。
		 * 名字只影响显示，引擎不认识任何一层（没有特殊行为）。 */
		constexpr uint32_t DefaultIndex = 0;
		constexpr uint32_t UIIndex = 1;
		constexpr uint32_t EffectIndex = 2;
		constexpr uint32_t WaterIndex = 3;
		constexpr uint32_t SkyIndex = 4;
		constexpr uint32_t TerrainIndex = 5;
		constexpr uint32_t BuiltinCount = 6;

		constexpr LayerMask None = 0u;
		constexpr LayerMask All = 0xFFFFFFFFu;
		/* 缺席 LayerComponent 的对象在这一层 */
		constexpr LayerMask Default = 1u << DefaultIndex;
		constexpr LayerMask UI = 1u << UIIndex;
		constexpr LayerMask Effect = 1u << EffectIndex;
		constexpr LayerMask Water = 1u << WaterIndex;
		constexpr LayerMask Sky = 1u << SkyIndex;
		constexpr LayerMask Terrain = 1u << TerrainIndex;

		/* 层名（UI 显示用）：内置层查名字表，其余按序号 */
		inline std::string GetName(uint32_t index)
		{
			switch (index)
			{
			case DefaultIndex: return "Default";
			case UIIndex:      return "UI";
			case EffectIndex:  return "Effect";
			case WaterIndex:   return "Water";
			case SkyIndex:     return "Sky";
			case TerrainIndex: return "Terrain";
			default:           return "Layer " + std::to_string(index);
			}
		}

		/* 对象对相机是否可见：两边掩码有交集即可 */
		inline bool IsVisible(LayerMask object_mask, LayerMask culling_mask)
		{
			return (object_mask & culling_mask) != 0;
		}
	}
}
