#include "Pch.h"
#include "TerrainAsset.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "Helios/Common/PathUtils.h"

namespace Helios::TerrainAsset
{
	namespace
	{
		/* 全部小端、逐字段写：不依赖结构体布局，Windows / macOS 一致 */
		struct Writer
		{
			std::vector<uint8_t>& Bytes;

			template <typename T>
			void Write(const T& value)
			{
				const auto* raw = reinterpret_cast<const uint8_t*>(&value);
				Bytes.insert(Bytes.end(), raw, raw + sizeof(T));
			}

			void WriteBytes(const void* data, size_t size)
			{
				const auto* raw = static_cast<const uint8_t*>(data);
				Bytes.insert(Bytes.end(), raw, raw + size);
			}
		};

		struct Reader
		{
			const uint8_t* Bytes{ nullptr };
			size_t Size{ 0 };
			size_t Offset{ 0 };

			bool Read(void* out, size_t size)
			{
				if (Offset + size > Size)
					return false;
				std::memcpy(out, Bytes + Offset, size);
				Offset += size;
				return true;
			}

			template <typename T>
			bool Read(T& out_value) { return Read(&out_value, sizeof(T)); }
		};

		uint16_t QuantizeHeight(float value)
		{
			const float clamped = glm::clamp(value, 0.0f, 1.0f);
			return static_cast<uint16_t>(clamped * 65535.0f + 0.5f);
		}
	}

	bool Data::IsValid() const
	{
		if (Resolution < kMinResolution || Resolution > kMaxResolution)
			return false;
		if (Extent.x <= 0.0f || Extent.y <= 0.0f || HeightScale < 0.0f)
			return false;
		if (Heights.size() != static_cast<size_t>(Resolution) * Resolution)
			return false;
		return true;
	}

	bool Encode(const Data& data, std::vector<uint8_t>& out_bytes)
	{
		if (!data.IsValid())
			return false;

		const uint32_t material_bytes = static_cast<uint32_t>(
			std::min<size_t>(data.MaterialPath.size(), kMaxMaterialPathBytes));

		Writer writer{ out_bytes };
		out_bytes.clear();
		out_bytes.reserve(8 + 4 * 4 + sizeof(glm::vec2) + 4 + 4 + material_bytes
			+ static_cast<size_t>(data.Resolution) * data.Resolution * sizeof(uint16_t));

		writer.WriteBytes(kMagic, sizeof(kMagic));
		writer.Write(kVersion);
		writer.Write(data.Resolution);
		writer.Write(data.Extent.x);
		writer.Write(data.Extent.y);
		writer.Write(data.HeightScale);
		writer.Write(material_bytes);
		if (material_bytes > 0)
			writer.WriteBytes(data.MaterialPath.data(), material_bytes);

		const uint32_t sample_count = data.Resolution * data.Resolution;
		writer.Write(sample_count);
		for (uint32_t index = 0; index < sample_count; ++index)
			writer.Write(QuantizeHeight(data.Heights[index]));
		return true;
	}

	bool Decode(const uint8_t* bytes, size_t size, Data& out_data)
	{
		if (bytes == nullptr || size < sizeof(kMagic))
			return false;

		Reader reader{ bytes, size, 0 };

		char magic[sizeof(kMagic)]{};
		if (!reader.Read(magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0)
			return false;

		uint32_t version = 0;
		if (!reader.Read(version) || version != kVersion)
			return false;

		Data data;
		uint32_t material_bytes = 0;
		if (!reader.Read(data.Resolution) || !reader.Read(data.Extent.x) || !reader.Read(data.Extent.y)
			|| !reader.Read(data.HeightScale) || !reader.Read(material_bytes))
			return false;

		if (material_bytes > kMaxMaterialPathBytes || material_bytes > size)
			return false;
		if (material_bytes > 0)
		{
			std::string path(material_bytes, '\0');
			if (!reader.Read(path.data(), material_bytes))
				return false;
			data.MaterialPath = std::move(path);
		}

		uint32_t sample_count = 0;
		if (!reader.Read(sample_count))
			return false;
		if (data.Resolution < kMinResolution || data.Resolution > kMaxResolution)
			return false;
		if (sample_count != data.Resolution * data.Resolution)
			return false;

		data.Heights.resize(sample_count);
		for (uint32_t index = 0; index < sample_count; ++index)
		{
			uint16_t raw = 0;
			if (!reader.Read(raw))
				return false;
			data.Heights[index] = static_cast<float>(raw) / 65535.0f;
		}

		if (!data.IsValid())
			return false;
		out_data = std::move(data);
		return true;
	}

	bool Write(const std::string& path, const Data& data)
	{
		if (path.empty() || !data.IsValid())
		{
			CORE_LOG_ERROR("TerrainAsset::Write: invalid input");
			return false;
		}

		std::filesystem::path file_path;
		if (!TryPathFromUtf8(path, file_path))
		{
			CORE_LOG_ERROR("TerrainAsset::Write: path is not valid UTF-8");
			return false;
		}

		std::vector<uint8_t> bytes;
		if (!Encode(data, bytes))
			return false;

		std::error_code error;
		if (file_path.has_parent_path())
			std::filesystem::create_directories(file_path.parent_path(), error);

		UniquePtr<FILE, decltype(&std::fclose)> handle(OpenUtf8File(file_path, "wb"), &std::fclose);
		if (handle == nullptr)
		{
			CORE_LOG_ERROR("TerrainAsset::Write: cannot open '{}'", path);
			return false;
		}

		if (std::fwrite(bytes.data(), 1, bytes.size(), handle.get()) != bytes.size())
		{
			CORE_LOG_ERROR("TerrainAsset::Write: incomplete write to '{}'", path);
			return false;
		}
		return true;
	}

	bool Read(const std::string& path, Data& out_data)
	{
		if (path.empty())
			return false;

		std::filesystem::path file_path;
		if (!TryPathFromUtf8(path, file_path))
			return false;

		UniquePtr<FILE, decltype(&std::fclose)> handle(OpenUtf8File(file_path, "rb"), &std::fclose);
		if (handle == nullptr)
		{
			CORE_LOG_WARN("TerrainAsset::Read: cannot open '{}'", path);
			return false;
		}

		std::vector<uint8_t> bytes;
		if (std::fseek(handle.get(), 0, SEEK_END) == 0)
		{
			const long size = std::ftell(handle.get());
			if (size <= 0)
				return false;
			bytes.resize(static_cast<size_t>(size));
			std::rewind(handle.get());
			if (std::fread(bytes.data(), 1, bytes.size(), handle.get()) != bytes.size())
				return false;
		}
		else
		{
			return false;
		}

		return Decode(bytes.data(), bytes.size(), out_data);
	}

	Data MakeDefault()
	{
		Data data;
		data.Resolution = 65;
		data.Extent = glm::vec2(128.0f);
		data.HeightScale = 40.0f;

		const uint32_t resolution = data.Resolution;
		data.Heights.resize(static_cast<size_t>(resolution) * resolution);

		constexpr float kTau = 6.2831853f;
		float lowest = 1.0f;
		for (uint32_t z = 0; z < resolution; ++z)
		{
			for (uint32_t x = 0; x < resolution; ++x)
			{
				const float u = static_cast<float>(x) / static_cast<float>(resolution - 1);
				const float v = static_cast<float>(z) / static_cast<float>(resolution - 1);
				float height = 0.5f
					+ 0.22f * std::sin(u * kTau) * std::sin(v * kTau)
					+ 0.08f * std::sin(u * kTau * 2.0f + 1.3f) * std::sin(v * kTau * 3.0f);
				height = glm::clamp(height, 0.0f, 1.0f);
				data.Heights[static_cast<size_t>(z) * resolution + x] = height;
				lowest = std::min(lowest, height);
			}
		}

		/* 跟程序化地形同一套语义：整体下移，最低点落在实体位置 */
		for (float& height : data.Heights)
			height -= lowest;

		return data;
	}
}
