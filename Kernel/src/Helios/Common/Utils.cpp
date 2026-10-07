#include "Pch.h"
#include "Utils.h"
#include "xxhash.h"
#include "stb_image.h"
#include <chrono>
#include <ctime>

namespace Helios
{
	/* Extract file base dir from path
	 * Example:
	 * path: "assets/scenes/test.scn"
	 * return: "assets/scenes/"
	 */
	std::string ExtractFileBaseDir(const std::string& path)
	{
		auto last_slash = path.find_last_of("/\\");
		last_slash = (last_slash == std::string::npos) ? 0 : last_slash + 1;
		return path.substr(0, last_slash);
	}

	/* Extract file base name from path
	 * Example:
	 * path: "assets/scenes/test.scn"
	 * return: "test.scn"
	 */
	std::string ExtractFileBaseName(const std::string& path)
	{
		const auto last_slash = path.find_last_of("/\\");
		return path.substr(last_slash + 1);
	}

	/* Extract file base dir & base name from path
	 * Example:
	 * path: "assets/scenes/test.scn"
	 * return: {"assets/scenes/", "test.scn"}
	 */
	std::pair<std::string, std::string> ExtractFileBaseDirAndBaseName(const std::string& path)
	{
		auto last_slash = path.find_last_of("/\\");
		last_slash = (last_slash == std::string::npos) ? 0 : last_slash + 1;
		return { path.substr(0, last_slash), path.substr(last_slash) };
	}

	/* Extract filename from path
	 * Examples:
	 * path: "assets/scenes/test.scn"
	 * return: "test"
	 */
	std::string ExtractFilename(const std::string& path)
	{
		auto last_slash = path.find_last_of("/\\");
		last_slash = (last_slash == std::string::npos) ? 0 : last_slash + 1;
		const auto last_dot = path.rfind('.');
		const auto count = (last_dot == std::string::npos) ? path.size() - last_slash : last_dot - last_slash;
		return path.substr(last_slash, count);
	}

	/* Extract filename from path
	 * Examples:
	 * path: "assets/scenes/test.scn"
	 * return: "scn"
	 */
	std::string ExtractFileSuffix(const std::string& path)
	{
		const auto last_dot = path.rfind('.');
		return path.substr(last_dot + 1);
	}

	/* Replace file suffix
	 * Examples:
	 * path: "assets/scenes/test.scn", suffix: ".bin"
	 * return: "assets/scenes/test.bin"
	 */
	std::string ReplaceFileSuffix(const std::string& path, const std::string& suffix)
	{
		const auto last_dot = path.rfind('.');
		return path.substr(0, last_dot) + suffix;
	}

	/* string to Id
	 * use xxHash
	 */
	uint32_t ToID(const std::string& value)
	{
		return XXH32(value.c_str(), value.size(), 0);
	}

	/* ============================ 文件信息展示 ============================ */

	std::string FormatFileSize(uintmax_t bytes)
	{
		if (bytes >= 1024ull * 1024ull * 1024ull)
			return ToString(static_cast<float>(bytes) / (1024.0f * 1024.0f * 1024.0f), 1) + " GB";
		if (bytes >= 1024ull * 1024ull)
			return ToString(static_cast<float>(bytes) / (1024.0f * 1024.0f), 1) + " MB";
		if (bytes >= 1024ull)
			return ToString(static_cast<float>(bytes) / 1024.0f, 1) + " KB";
		return std::to_string(bytes) + " B";
	}

	std::string FormatFileWriteTime(const std::filesystem::path& path)
	{
		std::error_code error;
		const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(path, error);
		if (error)
			return "unknown";

		/* file_time_type 的纪元由实现定义：两个"现在"之差可以把文件时间平移回 system_clock。
		 * 两者时长单位不同（libc++ 下 file clock 是纳秒），先折算再相加。 */
		const auto offset = std::chrono::duration_cast<std::chrono::system_clock::duration>(
			write_time - std::filesystem::file_time_type::clock::now());
		const std::time_t seconds = std::chrono::system_clock::to_time_t(
			std::chrono::system_clock::now() + offset);

		std::tm local_time{};
#ifdef PLATFORM_WINDOWS
		localtime_s(&local_time, &seconds);
#else
		localtime_r(&seconds, &local_time);
#endif

		char buffer[32] = {};
		std::strftime(buffer, sizeof(buffer), "%m-%d %H:%M", &local_time);
		return buffer;
	}

	bool QueryImageInfo(const std::string& path, int& out_width, int& out_height, int& out_channels)
	{
		return stbi_info(path.c_str(), &out_width, &out_height, &out_channels) != 0;
	}
}
