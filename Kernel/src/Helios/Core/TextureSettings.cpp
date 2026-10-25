#include "Pch.h"
#include "TextureSettings.h"
#include "Helios/Common/PathUtils.h"
#include <tinyxml2.h>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace Helios
{
	namespace
	{
		[[nodiscard]] bool EqualsIgnoreCase(const char* lhs, const char* rhs)
		{
			if (lhs == nullptr || rhs == nullptr)
				return false;

			while (*lhs != '\0' && *rhs != '\0')
			{
				if (std::tolower(static_cast<unsigned char>(*lhs)) != std::tolower(static_cast<unsigned char>(*rhs)))
					return false;
				++lhs;
				++rhs;
			}
			return *lhs == '\0' && *rhs == '\0';
		}

		[[nodiscard]] bool HasHdrExtension(const std::string& path)
		{
			std::string extension = PathToUtf8(PathFromUtf8(path).extension());
			for (char& character : extension)
				character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
			return extension == ".hdr";
		}

		/* 用途预设：只填采样、色彩、mip；数据布局（FlipV / SamplerType）由 .texmeta 的显式字段决定 */
		void ApplyUsagePreset(TextureLoadConfig& config, TexturePurpose usage)
		{
			switch (usage)
			{
			case TexturePurpose::Color:
				/* 材质着色器自己做了 SrgbToLinear，这里再开硬件 sRGB 会解码两次 */
				config.IsSrgb = false;
				config.IsGenMips = true;
				config.SamplerMinFilter = SamplerMinFilter::LinearMipmapLinear;
				config.SamplerMagFilter = SamplerMagFilter::Linear;
				config.SamplerWrapMode = SamplerWrapMode::Repeat;
				config.AnisoLevel = 8;
				break;
			case TexturePurpose::Data:
			case TexturePurpose::Normal:
				config.IsSrgb = false;
				config.IsGenMips = true;
				config.SamplerMinFilter = SamplerMinFilter::LinearMipmapLinear;
				config.SamplerMagFilter = SamplerMagFilter::Linear;
				config.SamplerWrapMode = SamplerWrapMode::Repeat;
				config.AnisoLevel = 8;
				break;
			case TexturePurpose::UI:
				config.IsSrgb = false;
				config.IsGenMips = false;
				config.SamplerMinFilter = SamplerMinFilter::Linear;
				config.SamplerMagFilter = SamplerMagFilter::Linear;
				config.SamplerWrapMode = SamplerWrapMode::ClampToEdge;
				config.AnisoLevel = 1;
				break;
			case TexturePurpose::Hdr:
				config.IsHdr = true;
				config.IsSrgb = false;
				config.IsGenMips = true;
				config.SamplerMinFilter = SamplerMinFilter::LinearMipmapLinear;
				config.SamplerMagFilter = SamplerMagFilter::Linear;
				config.SamplerWrapMode = SamplerWrapMode::ClampToEdge;
				config.AnisoLevel = 8;
				break;
			case TexturePurpose::Default:
			default:
				break;
			}
		}

		constexpr const char* kUsageNames[] = { "Default", "Color", "Data", "Normal", "UI", "HDR" };
		constexpr const char* kWrapNames[] = { "ClampToEdge", "Repeat", "MirroredRepeat" };
		constexpr const char* kMinFilterNames[] = {
			"Nearest", "Linear", "NearestMipmapNearest", "LinearMipmapNearest", "NearestMipmapLinear", "LinearMipmapLinear"
		};
		constexpr const char* kMagFilterNames[] = { "Nearest", "Linear" };
		constexpr const char* kSamplerTypeNames[] = { "2D", "2DArray", "CubeMap", "3D" };

		template<typename EnumType, size_t Count>
		[[nodiscard]] std::optional<EnumType> ParseEnum(const char* text, const char* const (&names)[Count])
		{
			if (text == nullptr)
				return std::nullopt;
			for (size_t index = 0; index < Count; ++index)
			{
				if (EqualsIgnoreCase(text, names[index]))
					return static_cast<EnumType>(index);
			}
			/* 顺手认几个常见写法 */
			if constexpr (std::is_same_v<EnumType, SamplerWrapMode>)
			{
				if (EqualsIgnoreCase(text, "Clamp"))
					return SamplerWrapMode::ClampToEdge;
				if (EqualsIgnoreCase(text, "Mirror"))
					return SamplerWrapMode::MirroredRepeat;
			}
			return std::nullopt;
		}

		[[nodiscard]] bool ReadTextFile(const std::filesystem::path& path, std::string& out_text)
		{
			auto file = std::unique_ptr<FILE, decltype(&std::fclose)>(OpenUtf8File(path, "rb"), &std::fclose);
			if (file == nullptr)
				return false;

			if (std::fseek(file.get(), 0, SEEK_END) != 0)
				return false;
			const long size = std::ftell(file.get());
			if (size < 0)
				return false;
			std::fseek(file.get(), 0, SEEK_SET);

			out_text.resize(static_cast<size_t>(size));
			return size == 0 || std::fread(out_text.data(), 1, out_text.size(), file.get()) == out_text.size();
		}

		[[nodiscard]] bool WriteTextFile(const std::filesystem::path& path, const std::string& text)
		{
			std::error_code error;
			if (!path.parent_path().empty())
				std::filesystem::create_directories(path.parent_path(), error);

			auto file = std::unique_ptr<FILE, decltype(&std::fclose)>(OpenUtf8File(path, "wb"), &std::fclose);
			if (file == nullptr)
				return false;
			return text.empty() || std::fwrite(text.data(), 1, text.size(), file.get()) == text.size();
		}
	}

	bool TextureSettings::IsEmpty() const
	{
		return !Usage.has_value()
			&& !Srgb.has_value()
			&& !WrapMode.has_value()
			&& !MinFilter.has_value()
			&& !MagFilter.has_value()
			&& !AnisoLevel.has_value()
			&& !Mipmaps.has_value()
			&& !FlipV.has_value()
			&& !SamplerType.has_value()
			&& !Hdr.has_value();
	}

	bool TextureSettings::operator==(const TextureSettings& other) const
	{
		return Usage == other.Usage
			&& Srgb == other.Srgb
			&& WrapMode == other.WrapMode
			&& MinFilter == other.MinFilter
			&& MagFilter == other.MagFilter
			&& AnisoLevel == other.AnisoLevel
			&& Mipmaps == other.Mipmaps
			&& FlipV == other.FlipV
			&& SamplerType == other.SamplerType
			&& Hdr == other.Hdr;
	}

	TextureSettingsStore& TextureSettingsStore::Instance()
	{
		static TextureSettingsStore instance;
		return instance;
	}

	std::string TextureSettingsStore::MetaPath(const std::string& texture_path)
	{
		return texture_path + ".texmeta";
	}

	bool TextureSettingsStore::IsMetaPath(const std::string& path)
	{
		std::string extension = PathToUtf8(PathFromUtf8(path).extension());
		for (char& character : extension)
			character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		return extension == ".texmeta";
	}

	TextureSettings TextureSettingsStore::Load(const std::string& texture_path)
	{
		std::lock_guard lock(m_Mutex);
		return Read(texture_path).Settings;
	}

	TextureSettingsStore::CacheEntry TextureSettingsStore::Read(const std::string& texture_path)
	{
		const std::filesystem::path meta_path = PathFromUtf8(MetaPath(texture_path));

		std::error_code error;
		const auto write_time = std::filesystem::last_write_time(meta_path, error);
		const bool exists = !error;

		auto iter = m_Cache.find(texture_path);
		if (iter != m_Cache.end() && iter->second.Valid && iter->second.Exists == exists
			&& (!exists || iter->second.WriteTime == write_time))
		{
			return iter->second;
		}

		CacheEntry entry;
		entry.Exists = exists;
		entry.WriteTime = exists ? write_time : std::filesystem::file_time_type{};
		entry.Valid = true;

		std::string text;
		if (exists && ReadTextFile(meta_path, text))
			entry.Settings = Parse(text);
		else if (exists)
			CORE_LOG_WARN("Failed to read texture settings: {}.", PathToUtf8(meta_path));

		m_Cache[texture_path] = entry;
		return entry;
	}

	TextureSettings TextureSettingsStore::Parse(const std::string& text)
	{
		TextureSettings settings;

		tinyxml2::XMLDocument document;
		if (document.Parse(text.c_str()) != tinyxml2::XML_SUCCESS)
		{
			CORE_LOG_WARN("Bad texture settings file, ignored.");
			return settings;
		}

		const tinyxml2::XMLElement* root = document.FirstChildElement("Texture");
		if (root == nullptr)
			return settings;

		settings.Usage = ParseEnum<TexturePurpose>(root->Attribute("Usage"), kUsageNames);
		settings.Srgb = root->BoolAttribute("Srgb") ? std::optional<bool>(true)
			: (root->FindAttribute("Srgb") != nullptr ? std::optional<bool>(false) : std::nullopt);
		settings.WrapMode = ParseEnum<SamplerWrapMode>(root->Attribute("Wrap"), kWrapNames);
		settings.MinFilter = ParseEnum<SamplerMinFilter>(root->Attribute("MinFilter"), kMinFilterNames);
		settings.MagFilter = ParseEnum<SamplerMagFilter>(root->Attribute("MagFilter"), kMagFilterNames);
		settings.Mipmaps = root->FindAttribute("Mipmaps") != nullptr
			? std::optional<bool>(root->BoolAttribute("Mipmaps")) : std::nullopt;
		settings.FlipV = root->FindAttribute("FlipV") != nullptr
			? std::optional<bool>(root->BoolAttribute("FlipV")) : std::nullopt;
		settings.SamplerType = ParseEnum<SamplerType>(root->Attribute("SamplerType"), kSamplerTypeNames);
		settings.Hdr = root->FindAttribute("Hdr") != nullptr
			? std::optional<bool>(root->BoolAttribute("Hdr")) : std::nullopt;

		if (root->FindAttribute("Aniso") != nullptr)
		{
			const int aniso = root->IntAttribute("Aniso", 8);
			settings.AnisoLevel = static_cast<uint8_t>(std::clamp(aniso, 1, 16));
		}

		return settings;
	}

	std::string TextureSettingsStore::Serialize(const TextureSettings& settings)
	{
		std::ostringstream stream;
		stream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Texture Version=\"1\"";

		if (settings.Usage.has_value())
			stream << " Usage=\"" << kUsageNames[static_cast<size_t>(*settings.Usage)] << "\"";
		if (settings.Srgb.has_value())
			stream << " Srgb=\"" << (*settings.Srgb ? "true" : "false") << "\"";
		if (settings.WrapMode.has_value())
			stream << " Wrap=\"" << kWrapNames[static_cast<size_t>(*settings.WrapMode)] << "\"";
		if (settings.MinFilter.has_value())
			stream << " MinFilter=\"" << kMinFilterNames[static_cast<size_t>(*settings.MinFilter)] << "\"";
		if (settings.MagFilter.has_value())
			stream << " MagFilter=\"" << kMagFilterNames[static_cast<size_t>(*settings.MagFilter)] << "\"";
		if (settings.AnisoLevel.has_value())
			stream << " Aniso=\"" << static_cast<int>(*settings.AnisoLevel) << "\"";
		if (settings.Mipmaps.has_value())
			stream << " Mipmaps=\"" << (*settings.Mipmaps ? "true" : "false") << "\"";
		if (settings.FlipV.has_value())
			stream << " FlipV=\"" << (*settings.FlipV ? "true" : "false") << "\"";
		if (settings.SamplerType.has_value())
			stream << " SamplerType=\"" << kSamplerTypeNames[static_cast<size_t>(*settings.SamplerType)] << "\"";
		if (settings.Hdr.has_value())
			stream << " Hdr=\"" << (*settings.Hdr ? "true" : "false") << "\"";

		stream << " />\n";
		return stream.str();
	}

	bool TextureSettingsStore::Save(const std::string& texture_path, const TextureSettings& settings)
	{
		std::lock_guard lock(m_Mutex);

		const std::string meta_path = MetaPath(texture_path);
		std::error_code error;
		if (settings.IsEmpty())
		{
			std::filesystem::remove(PathFromUtf8(meta_path), error);
			m_Cache.erase(texture_path);
			return !error;
		}

		if (!WriteTextFile(PathFromUtf8(meta_path), Serialize(settings)))
		{
			CORE_LOG_ERROR("Failed to write texture settings: {}.", meta_path);
			return false;
		}

		m_Cache.erase(texture_path);
		return true;
	}

	void TextureSettingsStore::Invalidate(const std::string& texture_path)
	{
		std::lock_guard lock(m_Mutex);
		m_Cache.erase(texture_path);
	}

	void TextureSettingsStore::Clear()
	{
		std::lock_guard lock(m_Mutex);
		m_Cache.clear();
	}

	TextureLoadConfig TextureSettingsStore::Resolve(const std::string& texture_path)
	{
		const TextureSettings settings = Load(texture_path);

		/* 起点是引擎默认值，再按用途预设铺一遍，最后让 .texmeta 的显式字段逐项覆盖 */
		TextureLoadConfig resolved;
		ApplyUsagePreset(resolved, settings.Usage.value_or(TexturePurpose::Default));
		resolved.IsHdr = HasHdrExtension(texture_path);

		if (settings.Srgb.has_value())
			resolved.IsSrgb = *settings.Srgb;
		if (settings.WrapMode.has_value())
			resolved.SamplerWrapMode = *settings.WrapMode;
		if (settings.MinFilter.has_value())
			resolved.SamplerMinFilter = *settings.MinFilter;
		if (settings.MagFilter.has_value())
			resolved.SamplerMagFilter = *settings.MagFilter;
		if (settings.AnisoLevel.has_value())
			resolved.AnisoLevel = *settings.AnisoLevel;
		if (settings.Mipmaps.has_value())
			resolved.IsGenMips = *settings.Mipmaps;
		if (settings.FlipV.has_value())
			resolved.IsFlipV = *settings.FlipV;
		if (settings.SamplerType.has_value())
			resolved.SamplerType = *settings.SamplerType;
		if (settings.Hdr.has_value())
			resolved.IsHdr = *settings.Hdr;

		resolved.AnisoLevel = static_cast<uint8_t>(std::clamp(static_cast<int>(resolved.AnisoLevel), 1, 16));
		return resolved;
	}
}
