#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>

#include "Helios/Common/Export.h"

namespace Helios
{
	/* .terrain 资产：高度图 + 覆盖范围 + 材质引用，存成自描述的二进制容器。
	 * 高度按 uint16 归一化（0..1），换算成世界高度的公式跟程序化地形同一套：
	 * 最低点落在实体位置，往上乘 HeightScale。 */
	namespace TerrainAsset
	{
		inline constexpr uint32_t kVersion = 1;
		inline constexpr char kMagic[8] = { 'H', 'E', 'L', 'T', 'E', 'R', 'R', 'N' };
		/* 分辨率上下限：下界要能成一个面，上界跟 TerrainField 的钳制一致 */
		inline constexpr uint32_t kMinResolution = 2;
		inline constexpr uint32_t kMaxResolution = 4096;
		/* 材质路径长度上限：防损坏文件撑出超大分配 */
		inline constexpr uint32_t kMaxMaterialPathBytes = 1024;

		struct Data
		{
			uint32_t Resolution{ 65 };
			glm::vec2 Extent{ 128.0f };   /* 局部半宽半长（米） */
			float HeightScale{ 40.0f };   /* 起伏幅度（米） */
			std::string MaterialPath;     /* 相对项目根；空 = 用内建默认材质 */
			std::vector<float> Heights;   /* Resolution² 个归一化高度，行优先 */

			[[nodiscard]] bool IsValid() const;
		};

		/* 编解码：编辑器的"新建资产"与烘焙都走它，不必落一次临时文件 */
		HELIOS_API bool Encode(const Data& data, std::vector<uint8_t>& out_bytes);
		HELIOS_API bool Decode(const uint8_t* bytes, size_t size, Data& out_data);

		HELIOS_API bool Write(const std::string& path, const Data& data);
		HELIOS_API bool Read(const std::string& path, Data& out_data);

		/* 新建资产的初始数据：平缓起伏，建出来一眼能看出是地形 */
		HELIOS_API Data MakeDefault();
	}
}
