#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace Helios::AstcFile
{
	/* astcenc 原生容器头：magic + 块尺寸 + 尺寸 + 深度，共 16 字节。
	 * 后端上传（MetalTexture）与属性面板预览读的都是这一份口径。 */
	constexpr uint32_t kMagic = 0x5CA1AB13;
	constexpr size_t kHeaderBytes = 16;

	struct Header
	{
		int BlockX{ 0 };
		int BlockY{ 0 };
		int Width{ 0 };
		int Height{ 0 };
		int Depth{ 0 };
	};

	inline uint32_t ReadU24(const unsigned char* bytes)
	{
		return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8)
			| (static_cast<uint32_t>(bytes[2]) << 16);
	}

	/* 只读 16 字节头，不碰块数据；不是 ASTC 或读不到返回 false */
	inline bool ReadHeader(const std::filesystem::path& path, Header& header)
	{
		unsigned char bytes[kHeaderBytes]{};
		std::ifstream file(path, std::ios::binary);
		if (!file || !file.read(reinterpret_cast<char*>(bytes), static_cast<std::streamsize>(kHeaderBytes)))
			return false;

		uint32_t magic = 0;
		for (int index = 0; index < 4; ++index)
			magic |= static_cast<uint32_t>(bytes[index]) << (8 * index);
		if (magic != kMagic)
			return false;

		header.BlockX = bytes[4];
		header.BlockY = bytes[5];
		header.Width = static_cast<int>(ReadU24(bytes + 7));
		header.Height = static_cast<int>(ReadU24(bytes + 10));
		header.Depth = static_cast<int>(ReadU24(bytes + 13));
		return header.Width > 0 && header.Height > 0;
	}
}
