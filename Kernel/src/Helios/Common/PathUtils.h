#pragma once

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Helios
{
	/* Application strings that represent paths are UTF-8; filesystem paths use the platform-native representation. */
	inline bool IsValidUtf8PathText(std::string_view text) noexcept
	{
		for (size_t index = 0; index < text.size();)
		{
			const auto first = static_cast<unsigned char>(text[index]);
			if (first == 0)
				return false;
			if (first <= 0x7f)
			{
				++index;
				continue;
			}

			size_t continuation_count = 0;
			unsigned char second_min = 0x80;
			unsigned char second_max = 0xbf;
			if (first >= 0xc2 && first <= 0xdf)
				continuation_count = 1;
			else if (first == 0xe0)
			{
				continuation_count = 2;
				second_min = 0xa0;
			}
			else if ((first >= 0xe1 && first <= 0xec) || (first >= 0xee && first <= 0xef))
				continuation_count = 2;
			else if (first == 0xed)
			{
				continuation_count = 2;
				second_max = 0x9f;
			}
			else if (first == 0xf0)
			{
				continuation_count = 3;
				second_min = 0x90;
			}
			else if (first >= 0xf1 && first <= 0xf3)
				continuation_count = 3;
			else if (first == 0xf4)
			{
				continuation_count = 3;
				second_max = 0x8f;
			}
			else
				return false;

			if (index + continuation_count >= text.size())
				return false;
			const auto second = static_cast<unsigned char>(text[index + 1]);
			if (second < second_min || second > second_max)
				return false;
			for (size_t offset = 2; offset <= continuation_count; ++offset)
			{
				const auto byte = static_cast<unsigned char>(text[index + offset]);
				if (byte < 0x80 || byte > 0xbf)
					return false;
			}
			index += continuation_count + 1;
		}
		return true;
	}

	inline bool TryPathFromUtf8(std::string_view utf8, std::filesystem::path& out_path) noexcept
	{
		if (!IsValidUtf8PathText(utf8))
			return false;

		try
		{
			std::u8string encoded;
			encoded.reserve(utf8.size());
			for (unsigned char byte : utf8)
				encoded.push_back(static_cast<char8_t>(byte));
			out_path = std::filesystem::path(encoded);
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	inline std::filesystem::path PathFromUtf8(std::string_view utf8)
	{
		std::filesystem::path path;
		if (!TryPathFromUtf8(utf8, path))
			throw std::invalid_argument("Invalid UTF-8 filesystem path");
		return path;
	}

	inline std::string PathToUtf8(const std::filesystem::path& path)
	{
		const std::u8string encoded = path.generic_u8string();
		std::string utf8;
		utf8.reserve(encoded.size());
		for (char8_t byte : encoded)
			utf8.push_back(static_cast<char>(byte));
		return utf8;
	}

	inline bool TryPathFromUtf8Payload(const void* data, size_t data_size, std::filesystem::path& out_path) noexcept
	{
		if (data == nullptr || data_size < 2)
			return false;

		const auto* bytes = static_cast<const char*>(data);
		if (bytes[data_size - 1] != '\0')
			return false;
		for (size_t index = 0; index + 1 < data_size; ++index)
			if (bytes[index] == '\0')
				return false;

		return TryPathFromUtf8(std::string_view(bytes, data_size - 1), out_path);
	}

	inline FILE* OpenUtf8File(const std::filesystem::path& path, const char* mode)
	{
		if (path.empty() || mode == nullptr)
			return nullptr;

#if defined(_WIN32) || defined(PLATFORM_WINDOWS)
		std::wstring wide_mode;
		for (const unsigned char character : std::string_view(mode))
		{
			if (character > 0x7f)
				return nullptr;
			wide_mode.push_back(static_cast<wchar_t>(character));
		}
		return _wfopen(path.c_str(), wide_mode.c_str());
#else
		return std::fopen(path.c_str(), mode);
#endif
	}
}
