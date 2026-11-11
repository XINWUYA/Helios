#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "Helios/Common/Export.h"

namespace Helios
{
	/* 统一文件读取。桌面与 iOS 都按路径直读 —— .app 里的文件可以直接打开；
	 * Android 的 assets 在 APK 里是 zip 条目，std::ifstream 打不开，将来在这里换成
	 * 平台接口，调用方不用跟着改。 */
	namespace FileSystem
	{
		/* 读整个文件。失败时 out 保持原样，返回 false */
		HELIOS_API bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& out);

		/* 读文本（着色器、配置、场景这类）。二进制模式读，不翻译换行 */
		HELIOS_API bool ReadText(const std::filesystem::path& path, std::string& out);
	}
}
