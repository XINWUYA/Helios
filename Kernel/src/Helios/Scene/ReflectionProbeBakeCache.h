#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Helios/Renderer/RenderCommon.h"

namespace Helios::ReflectionProbeBakeCache
{
	/* 反射探针烘焙结果的磁盘缓存：若干 HDR 立方体贴图（RGBA16F）逐 mip、逐面写进自描述的二进制
	 * 容器 —— 场景只记文件路径，加载直接恢复。Header（magic | version | count）+ 每图
	 * （kind | format | size | mips；每 mip face_byte_size + 6 面 texel）。只负责字节流。 */

	inline constexpr uint32_t kVersion = 1;
	inline constexpr char kMagic[8] = { 'H', 'E', 'L', 'I', 'O', 'S', 'P', 'B' };
	inline constexpr uint32_t kFaceCount = 6;
	/* mip 层数的合理上限，用于抵挡损坏文件造成的超大分配 */
	inline constexpr uint32_t kMaxMipLevels = 32;

	/* 一张立方体贴图在缓存里的位置 */
	enum class ImageKind : uint32_t
	{
		Environment = 0,   /* 环境立方图（场景捕获 / 天空盒展开的结果） */
		Irradiance = 1,    /* 漫反射辐照度 */
		Prefilter = 2,     /* 预滤波（按粗糙度分级） */
	};

	struct Image
	{
		ImageKind     Kind{ ImageKind::Environment };
		TextureFormat Format{ TextureFormat::RGBA16F };
		uint32_t      Size{ 0 };   /* mip0 的边长 */
		/* Mips[mip][face] = 该面该层的原始 texel 字节 */
		std::vector<std::vector<std::vector<uint8_t>>> Mips;
	};

	struct Data
	{
		/* 为空表示没有任何可保存的内容 */
		std::vector<Image> Images;

		[[nodiscard]] bool IsValid() const
		{
			for (const Image& image : Images)
			{
				if (image.Size == 0 || image.Mips.empty())
					return false;
			}
			return !Images.empty();
		}
	};

	/* 写出缓存文件；目录不存在时会尝试创建 */
	bool Write(const std::string& path, const Data& data);
	/* 读入缓存文件；magic / 版本 / 尺寸校验不通过时返回 false 并保持 out_data 不变 */
	bool Read(const std::string& path, Data& out_data);
}
