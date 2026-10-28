#pragma once
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include "Helios/Common/PathUtils.h"

namespace Helios
{
	/* 编辑器里所有"不分大小写"的文本比较都走这里：搜索框过滤、后缀判断。
	 * 只折叠 ASCII 字母，UTF-8 的高位字节原样比较，中文照样能搜。 */

	inline char LowerAscii(char character)
	{
		return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
	}

	inline std::string ToLowercase(std::string_view text)
	{
		std::string result(text);
		for (char& character : result)
			character = LowerAscii(character);
		return result;
	}

	inline std::string ToLowercase(const char* text)
	{
		return (text != nullptr) ? ToLowercase(std::string_view(text)) : std::string();
	}

	/* 后缀的小写形式（带点，如 ".glsl"）：判断文件类型统一用它 */
	inline std::string LowercaseExtension(const std::filesystem::path& path)
	{
		return ToLowercase(PathToUtf8(path.extension()));
	}

	/* needle 必须是已经小写化的（用 ToLowercase 过一次）；needle 为空视为全部命中。
	 * 边比边转小写，不复制原串：搜索框每帧对大量条目调用 */
	inline bool ContainsCaseInsensitive(std::string_view haystack, std::string_view lowercase_needle)
	{
		if (lowercase_needle.empty())
			return true;

		return std::search(haystack.begin(), haystack.end(), lowercase_needle.begin(), lowercase_needle.end(),
			[](char lhs, char rhs) { return LowerAscii(lhs) == rhs; }) != haystack.end();
	}

	inline bool ContainsCaseInsensitive(const char* haystack, std::string_view lowercase_needle)
	{
		return (haystack != nullptr) && ContainsCaseInsensitive(std::string_view(haystack), lowercase_needle);
	}
}
