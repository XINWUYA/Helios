#include "Pch.h"
#include "FileSystem.h"

namespace Helios
{
	namespace FileSystem
	{
		bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& out)
		{
			if (path.empty())
				return false;

			std::ifstream stream(path, std::ios::in | std::ios::binary | std::ios::ate);
			if (!stream.is_open())
				return false;

			const std::streamoff size = stream.tellg();
			if (size < 0)
				return false;

			std::vector<uint8_t> buffer(static_cast<size_t>(size));
			stream.seekg(0, std::ios::beg);
			if (size > 0 && !stream.read(reinterpret_cast<char*>(buffer.data()), size))
				return false;

			out = std::move(buffer);
			return true;
		}

		bool ReadText(const std::filesystem::path& path, std::string& out)
		{
			if (path.empty())
				return false;

			std::ifstream stream(path, std::ios::in | std::ios::binary);
			if (!stream.is_open())
				return false;

			std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
			out = std::move(text);
			return true;
		}
	}
}
