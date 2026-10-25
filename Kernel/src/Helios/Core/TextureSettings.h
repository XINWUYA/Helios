#pragma once
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include "Helios/Common/Common.h"
#include "Helios/VirtualDevice/DeviceTexture.h"

namespace Helios
{
	/* 贴图用途：只用来决定其余字段的默认值，不进 GPU。
	 * Default 表示没写用途，行为和没有 .texmeta 时一致。 */
	enum class TexturePurpose : uint8_t
	{
		Default,
		Color,
		Data,
		Normal,
		UI,
		Hdr
	};

	/* .texmeta 的增量内容：字段为 nullopt 表示"用默认值"，不写进文件 */
	struct HELIOS_API TextureSettings
	{
		std::optional<TexturePurpose> Usage;
		std::optional<bool> Srgb;							/* nullopt = 跟随用途 */
		std::optional<SamplerWrapMode> WrapMode;
		std::optional<SamplerMinFilter> MinFilter;
		std::optional<SamplerMagFilter> MagFilter;
		std::optional<uint8_t> AnisoLevel;
		std::optional<bool> Mipmaps;
		std::optional<bool> FlipV;
		std::optional<SamplerType> SamplerType;
		std::optional<bool> Hdr;							/* nullopt = 按后缀推断 */

		[[nodiscard]] bool IsEmpty() const;
		bool operator==(const TextureSettings& other) const;
		bool operator!=(const TextureSettings& other) const { return !(*this == other); }
	};

	/* 贴图设置仓库：按路径读写 <贴图>.texmeta，并把资产设置解析成全量值。
	 * 只有这里知道 .texmeta 的存在，加载器和后端都不直接碰文件。 */
	class HELIOS_API TextureSettingsStore
	{
	public:
		static TextureSettingsStore& Instance();

		/* 贴图路径 -> 伴生文件路径（"a.png" -> "a.png.texmeta"） */
		static std::string MetaPath(const std::string& texture_path);
		/* 是不是伴生文件（资源浏览器据此不把它当资产列出来） */
		static bool IsMetaPath(const std::string& path);

		/* 读设置；文件不存在或解析失败返回空设置 */
		TextureSettings Load(const std::string& texture_path);
		/* 写设置：只写显式字段；全部为空时删掉文件。返回是否落盘成功 */
		bool Save(const std::string& texture_path, const TextureSettings& settings);
		/* 丢掉某张图的缓存（保存后调用，下次 Load 重新读盘） */
		void Invalidate(const std::string& texture_path);
		void Clear();

		/* .texmeta + 用途预设 + 引擎默认值 -> 全量有效配置。
		 * 贴图设置是加载配置的唯一来源，引用点不再提供自己的配置。 */
		TextureLoadConfig Resolve(const std::string& texture_path);

	private:
		TextureSettingsStore() = default;

		struct CacheEntry
		{
			TextureSettings Settings;
			std::filesystem::file_time_type WriteTime{};
			bool Exists{ false };
			bool Valid{ false };
		};

		CacheEntry Read(const std::string& texture_path);
		static TextureSettings Parse(const std::string& text);
		static std::string Serialize(const TextureSettings& settings);

		std::unordered_map<std::string, CacheEntry> m_Cache;
		std::mutex m_Mutex;
	};
}
