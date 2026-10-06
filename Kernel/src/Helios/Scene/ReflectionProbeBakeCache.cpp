#include "Pch.h"
#include "ReflectionProbeBakeCache.h"
#include "Helios/Common/PathUtils.h"
#include <filesystem>
#include <fstream>

namespace Helios::ReflectionProbeBakeCache
{
	namespace
	{
		/* 逐字段读写裸字节，避免依赖结构体布局 */
		template <typename T>
		bool WriteScalar(std::ofstream& stream, const T& value)
		{
			stream.write(reinterpret_cast<const char*>(&value), sizeof(T));
			return stream.good();
		}

		template <typename T>
		bool ReadScalar(std::ifstream& stream, T& out_value)
		{
			stream.read(reinterpret_cast<char*>(&out_value), sizeof(T));
			return stream.good();
		}

		/* 该 mip 层级某一面的期望字节数；格式无固定 texel 尺寸（块压缩）时返回 0 表示不校验 */
		size_t ExpectedFaceByteSize(TextureFormat format, uint32_t size, uint32_t mip_level)
		{
			const uint32_t texel_size = GetTextureFormatTexelSize(format);
			if (texel_size == 0)
				return 0;

			const uint32_t level_size = std::max(1u, size >> mip_level);
			return static_cast<size_t>(level_size) * level_size * texel_size;
		}
	}

	bool Write(const std::string& path, const Data& data)
	{
		if (path.empty() || !data.IsValid())
		{
			CORE_LOG_ERROR("ReflectionProbeBakeCache::Write: invalid input");
			return false;
		}

		std::filesystem::path file_path;
		if (!TryPathFromUtf8(path, file_path))
		{
			CORE_LOG_ERROR("ReflectionProbeBakeCache::Write: path is not valid UTF-8");
			return false;
		}

		std::error_code error;
		if (file_path.has_parent_path())
			std::filesystem::create_directories(file_path.parent_path(), error);

		std::ofstream stream(file_path, std::ios::binary | std::ios::trunc);
		if (!stream.is_open())
		{
			CORE_LOG_ERROR("ReflectionProbeBakeCache::Write: cannot open '{}'", path);
			return false;
		}

		stream.write(kMagic, sizeof(kMagic));

		const uint32_t version = kVersion;
		const uint32_t image_count = static_cast<uint32_t>(data.Images.size());
		if (!WriteScalar(stream, version) || !WriteScalar(stream, image_count))
			return false;

		for (const Image& image : data.Images)
		{
			const uint32_t kind = static_cast<uint32_t>(image.Kind);
			const uint32_t format = static_cast<uint32_t>(image.Format);
			const uint32_t mip_levels = static_cast<uint32_t>(image.Mips.size());

			if (!WriteScalar(stream, kind) || !WriteScalar(stream, format)
				|| !WriteScalar(stream, image.Size) || !WriteScalar(stream, mip_levels))
				return false;

			for (const auto& faces : image.Mips)
			{
				if (faces.size() != kFaceCount)
				{
					CORE_LOG_ERROR("ReflectionProbeBakeCache::Write: expected {} faces, got {}", kFaceCount, faces.size());
					return false;
				}

				const uint32_t face_byte_size = static_cast<uint32_t>(faces[0].size());
				if (!WriteScalar(stream, face_byte_size))
					return false;

				for (const auto& face : faces)
				{
					if (face.size() != face_byte_size)
					{
						CORE_LOG_ERROR("ReflectionProbeBakeCache::Write: inconsistent face sizes");
						return false;
					}

					stream.write(reinterpret_cast<const char*>(face.data()), static_cast<std::streamsize>(face.size()));
					if (!stream.good())
						return false;
				}
			}
		}

		stream.flush();
		const bool succeeded = stream.good();
		if (!succeeded)
			CORE_LOG_ERROR("ReflectionProbeBakeCache::Write: failed while writing '{}'", path);

		return succeeded;
	}

	bool Read(const std::string& path, Data& out_data)
	{
		if (path.empty())
			return false;

		std::filesystem::path file_path;
		if (!TryPathFromUtf8(path, file_path))
		{
			CORE_LOG_ERROR("ReflectionProbeBakeCache::Read: path is not valid UTF-8");
			return false;
		}

		std::ifstream stream(file_path, std::ios::binary);
		if (!stream.is_open())
		{
			CORE_LOG_ERROR("ReflectionProbeBakeCache::Read: cannot open '{}'", path);
			return false;
		}

		char magic[sizeof(kMagic)] = {};
		stream.read(magic, sizeof(magic));
		if (!stream.good() || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0)
		{
			CORE_LOG_ERROR("ReflectionProbeBakeCache::Read: '{}' is not a probe bake cache", path);
			return false;
		}

		uint32_t version = 0;
		uint32_t image_count = 0;
		if (!ReadScalar(stream, version) || !ReadScalar(stream, image_count))
			return false;

		if (version != kVersion)
		{
			CORE_LOG_ERROR("ReflectionProbeBakeCache::Read: unsupported version {} (expected {})", version, kVersion);
			return false;
		}

		Data loaded;
		loaded.Images.reserve(image_count);

		for (uint32_t image_index = 0; image_index < image_count; ++image_index)
		{
			uint32_t kind = 0;
			uint32_t format = 0;
			uint32_t size = 0;
			uint32_t mip_levels = 0;
			if (!ReadScalar(stream, kind) || !ReadScalar(stream, format)
				|| !ReadScalar(stream, size) || !ReadScalar(stream, mip_levels))
				return false;

			if (size == 0 || mip_levels == 0 || mip_levels > kMaxMipLevels)
			{
				CORE_LOG_ERROR("ReflectionProbeBakeCache::Read: invalid image header in '{}'", path);
				return false;
			}

			Image image;
			image.Kind = static_cast<ImageKind>(kind);
			image.Format = static_cast<TextureFormat>(format);
			image.Size = size;
			image.Mips.resize(mip_levels);

			for (uint32_t mip = 0; mip < mip_levels; ++mip)
			{
				uint32_t face_byte_size = 0;
				if (!ReadScalar(stream, face_byte_size) || face_byte_size == 0)
					return false;

				/* 与头部声明的尺寸比对，挡住损坏文件造成的超大分配 */
				const size_t expected = ExpectedFaceByteSize(image.Format, size, mip);
				if (expected != 0 && expected != face_byte_size)
				{
					CORE_LOG_ERROR("ReflectionProbeBakeCache::Read: mip {} face size mismatch in '{}'", mip, path);
					return false;
				}

				auto& faces = image.Mips[mip];
				faces.resize(kFaceCount);
				for (auto& face : faces)
				{
					face.resize(face_byte_size);
					stream.read(reinterpret_cast<char*>(face.data()), static_cast<std::streamsize>(face_byte_size));
					if (!stream.good())
						return false;
				}
			}

			loaded.Images.emplace_back(std::move(image));
		}

		out_data = std::move(loaded);
		return true;
	}
}
