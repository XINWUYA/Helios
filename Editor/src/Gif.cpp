#include "Pch.h"
#include "Gif.h"
#include "Helios/Common/PathUtils.h"
#include "Helios/Renderer/Renderer.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>
#include <stb_image.h>

namespace Helios::Gif
{
	namespace
	{
		/* 帧数 × 单帧高超过它就不做图集（Metal/GL 的纹理尺寸上限内），调用方回落静态首帧 */
		constexpr int kMaxAtlasHeight = 8192;
		/* 延时缺省 / 极短（<20ms）按 100ms 兜底，与浏览器的节流口径一致 */
		constexpr float kMinFrameSeconds = 0.1f;

		bool IsGifPath(const std::filesystem::path& path)
		{
			std::string extension = PathToUtf8(path.extension());
			for (char& character : extension)
				character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
			return extension == ".gif";
		}

		/* 整个文件读进内存（stb 的动图接口只吃内存缓冲）；失败返回 false */
		bool ReadAllBytes(const std::filesystem::path& path, std::vector<unsigned char>& out_bytes)
		{
			auto file = std::unique_ptr<FILE, decltype(&std::fclose)>(OpenUtf8File(path, "rb"), &std::fclose);
			if (file == nullptr)
				return false;

			if (std::fseek(file.get(), 0, SEEK_END) != 0)
				return false;
			const long size = std::ftell(file.get());
			if (size <= 0 || std::fseek(file.get(), 0, SEEK_SET) != 0)
				return false;

			out_bytes.resize(static_cast<size_t>(size));
			return std::fread(out_bytes.data(), 1, out_bytes.size(), file.get()) == out_bytes.size();
		}

		/* 解码成帧表 + 建图集纹理；解析不了返回空 */
		SharedPtr<const Clip> LoadClip(const std::filesystem::path& path)
		{
			std::vector<unsigned char> bytes;
			if (!ReadAllBytes(path, bytes))
				return nullptr;

			/* 与静态贴图同一套约定：行序垂直翻转（TextureLoadConfig 的默认值），
			 * 显示端按 (0,1)-(1,0) 采回来 */
			stbi_set_flip_vertically_on_load(1);

			int* delays = nullptr;
			int width = 0;
			int height = 0;
			int frames = 0;
			int channels = 0;
			stbi_uc* pixels = stbi_load_gif_from_memory(bytes.data(), static_cast<int>(bytes.size()),
				&delays, &width, &height, &frames, &channels, 4);
			if (pixels == nullptr || width <= 0 || height <= 0 || frames <= 0)
			{
				free(delays);
				stbi_image_free(pixels);
				return nullptr;
			}

			auto clip = CreateSharedPtr<Clip>();
			clip->FrameCount = frames;
			clip->FrameWidth = static_cast<float>(width);
			clip->FrameHeight = static_cast<float>(height);
			clip->FrameStarts.reserve(static_cast<size_t>(frames) + 1);
			clip->FrameStarts.push_back(0.0f);
			for (int frame = 0; frame < frames; ++frame)
			{
				/* stb 给的延时是毫秒（GIF 的 1/100s 乘以 10） */
				const int delay_ms = (delays != nullptr) ? delays[frame] : 0;
				const float seconds = (delay_ms < 20) ? kMinFrameSeconds : static_cast<float>(delay_ms) / 1000.0f;
				clip->FrameStarts.push_back(clip->FrameStarts.back() + seconds);
			}
			free(delays);

			/* 图集：帧竖排（帧 0 在最上）。没有渲染后端或整体太高就不建，回落静态首帧 */
			const int64_t atlas_height = static_cast<int64_t>(height) * frames;
			if (atlas_height <= kMaxAtlasHeight && Renderer::GetRenderAPI() != nullptr)
			{
				TextureDesc desc;
				desc.Width = static_cast<uint32_t>(width);
				desc.Height = static_cast<uint32_t>(atlas_height);
				desc.Format = TextureFormat::RGBA8;
				desc.SamplerType = SamplerType::Sampler2D;
				desc.Usage = TextureUsage::Sampleable;
				desc.MipLevels = 1;

				clip->Texture = DeviceTexture::Create("GifPreview_" + PathToUtf8(path.filename()), desc);
				if (clip->Texture != nullptr)
					clip->Texture->SetData(pixels, PixelDesc{ PixelFormat::RGBA, PixelType::UnsignedByte });
			}

			stbi_image_free(pixels);
			return clip;
		}
	}

	SharedPtr<const Clip> GetClip(const std::filesystem::path& path)
	{
		/* 只服务 UI 帧；缓存按文件写入时间失效 —— 外部改过文件，下一帧就重新解析 */
		struct CacheEntry
		{
			std::filesystem::file_time_type WriteTime{};
			SharedPtr<const Clip> Parsed;
		};
		static std::unordered_map<std::string, CacheEntry> s_Cache;

		if (!IsGifPath(path))
			return nullptr;

		std::error_code error;
		const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(path, error);
		if (error)
		{
			s_Cache.erase(PathToUtf8(path));
			return nullptr;
		}

		const std::string key = PathToUtf8(path);
		const auto found = s_Cache.find(key);
		if (found != s_Cache.end() && found->second.WriteTime == write_time)
			return found->second.Parsed;

		SharedPtr<const Clip> parsed = LoadClip(path);
		s_Cache[key] = CacheEntry{ write_time, parsed };
		return parsed;
	}

	int FrameAt(const Clip& clip, float seconds)
	{
		if (clip.FrameCount <= 0 || clip.FrameStarts.size() < 2)
			return 0;

		const float total = clip.FrameStarts.back();
		if (total <= 0.0f)
			return 0;

		float time = std::fmod(seconds, total);
		if (time < 0.0f)
			time += total;

		for (int frame = clip.FrameCount - 1; frame >= 0; --frame)
		{
			if (time >= clip.FrameStarts[static_cast<size_t>(frame)])
				return frame;
		}
		return 0;
	}

	void FrameUv(const Clip& clip, int frame, ImVec2& uv0, ImVec2& uv1)
	{
		const int count = ImMax(clip.FrameCount, 1);
		frame = ImClamp(frame, 0, count - 1);

		/* 整张图上下翻转采样（同静态缩略图）：帧 k 的画面顶在 v=(k+1)/N 那一侧。
		 * 再收半纹素，别把邻帧的行采进来。 */
		const float inset = 0.5f / (static_cast<float>(count) * ImMax(clip.FrameHeight, 1.0f));
		uv0 = ImVec2(0.0f, (static_cast<float>(frame) + 1.0f) / count - inset);
		uv1 = ImVec2(1.0f, static_cast<float>(frame) / count + inset);
	}
}
