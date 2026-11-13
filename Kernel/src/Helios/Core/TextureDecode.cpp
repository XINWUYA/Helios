#include "Pch.h"
#include "TextureDecode.h"
#include "FileSystem.h"
#include "Jobs.h"
#include "TextureSettings.h"
#include "Helios/Common/AssetPath.h"
#include "Helios/Common/PathUtils.h"
#include "Helios/Renderer/RenderCommon.h"
#include "Helios/Renderer/Renderer.h"
#include <tinyxml2.h>

/* stb 的实现编进引擎库里，Editor 侧也要直接调用（非 Windows 默认可见，收紧可见性时要显式放开）。
 * 放在这里而不是 GL 后端：解图跟图形 API 无关，移动端只有 Metal 也得有这一份。 */
#if !defined(PLATFORM_WINDOWS)
#pragma GCC visibility push(default)
#endif
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#if !defined(PLATFORM_WINDOWS)
#pragma GCC visibility pop
#endif

#include <condition_variable>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace Helios
{
	DecodedImage::~DecodedImage()
	{
		if (Pixels != nullptr)
			stbi_image_free(Pixels);
	}

	DecodedImage::DecodedImage(DecodedImage&& other) noexcept
		: Pixels(other.Pixels), Width(other.Width), Height(other.Height), IsFloat(other.IsFloat)
	{
		other.Pixels = nullptr;
		other.Width = 0;
		other.Height = 0;
		other.IsFloat = false;
	}

	DecodedImage& DecodedImage::operator=(DecodedImage&& other) noexcept
	{
		if (this == &other)
			return *this;

		if (Pixels != nullptr)
			stbi_image_free(Pixels);

		Pixels = other.Pixels;
		Width = other.Width;
		Height = other.Height;
		IsFloat = other.IsFloat;

		other.Pixels = nullptr;
		other.Width = 0;
		other.Height = 0;
		other.IsFloat = false;
		return *this;
	}

	namespace
	{
		/* 一张图在后台解码期间的状态：Task 的终态说了算，
		 * 跑到 Completed 之后 Image / Failed 才可读（跳过就是没有结果） */
		struct Slot
		{
			bool Failed{ false };
			DecodedImage Image{};
			Jobs::Task Task{};
		};

		std::mutex s_SlotsMutex;
		std::unordered_map<std::string, SharedPtr<Slot>> s_Slots;

		/* 真正解码：配置从贴图设置解析，跟 DeviceTexture::Create 走同一份口径 */
		bool DecodeNow(const std::string& path, DecodedImage& out)
		{
			const TextureLoadConfig config = TextureSettingsStore::Instance().Resolve(path);

			/* 线程局部开关：并行解码时不能动 stb 的全局翻转 */
			stbi_set_flip_vertically_on_load_thread(config.IsFlipV ? 1 : 0);

			/* 先读进内存再交给 stb：怎么读到字节由 FileSystem 决定，stb 只认缓冲 */
			std::filesystem::path file_path;
			std::vector<uint8_t> bytes;
			if (!TryPathFromUtf8(path, file_path) || !FileSystem::ReadFile(file_path, bytes) || bytes.empty())
				return false;

			int width = 0;
			int height = 0;
			int channels = 0;
			const int byte_count = static_cast<int>(bytes.size());
			void* pixels = config.IsHdr
				? static_cast<void*>(stbi_loadf_from_memory(bytes.data(), byte_count, &width, &height, &channels, 4))
				: static_cast<void*>(stbi_load_from_memory(bytes.data(), byte_count, &width, &height, &channels, 4));
			if (pixels == nullptr)
				return false;

			out.Pixels = pixels;
			out.Width = width;
			out.Height = height;
			out.IsFloat = config.IsHdr;
			return true;
		}
	}

	void TextureDecode::Prefetch(const std::string& path)
	{
		/* 目前只有 Metal 后端在加载时来取结果，别的后端别白解一遍 */
		if (path.empty() || Renderer::CurrentAPI() != RenderAPI::Metal || !Jobs::IsInitialized())
			return;

		std::lock_guard lock(s_SlotsMutex);
		if (s_Slots.find(path) != s_Slots.end())
			return;

		auto slot = CreateSharedPtr<Slot>();
		s_Slots.emplace(path, slot);

		slot->Task = Jobs::RunBackground([slot, path]
		{
			DecodedImage image;
			const bool ok = DecodeNow(path, image);
			slot->Image = std::move(image);
			slot->Failed = !ok;
		});
	}

	void TextureDecode::Prefetch(const std::vector<std::string>& paths)
	{
		for (const std::string& path : paths)
			Prefetch(path);
	}

	void TextureDecode::PrefetchMaterialTextures(const std::string& material_asset_path)
	{
		if (material_asset_path.empty())
			return;

		tinyxml2::XMLDocument document;
		if (document.LoadFile(PathToUtf8(ResolveAssetPath(PathFromUtf8(material_asset_path))).c_str())
			!= tinyxml2::XML_SUCCESS)
			return;

		const tinyxml2::XMLElement* root = document.RootElement();
		const tinyxml2::XMLElement* parameters = root != nullptr ? root->FirstChildElement("Parameters") : nullptr;
		if (parameters == nullptr)
			return;

		std::vector<std::string> paths;
		for (const tinyxml2::XMLElement* param = parameters->FirstChildElement("ParamInfo");
			param != nullptr; param = param->NextSiblingElement("ParamInfo"))
		{
			/* 只看类型是贴图的参数，值就是资产相对路径 */
			if (param->IntAttribute("Type", -1) != static_cast<int>(ParamType::Texture))
				continue;

			const char* value = param->Attribute("Value");
			if (value != nullptr && *value != '\0')
				paths.push_back(PathToUtf8(ResolveAssetPath(PathFromUtf8(value))));
		}

		Prefetch(paths);
	}

	bool TextureDecode::Take(const std::string& path, DecodedImage& out)
	{
		SharedPtr<Slot> slot;
		{
			std::lock_guard lock(s_SlotsMutex);
			const auto it = s_Slots.find(path);
			if (it == s_Slots.end())
				return false;

			slot = it->second;
			s_Slots.erase(it);
		}

		/* 等任务结束：跑完或跳过都会返回；跳过说明这张图没解出来 */
		slot->Task.Wait();
		if (slot->Task.State() != Jobs::TaskState::Completed || slot->Failed)
			return false;

		out = std::move(slot->Image);
		return true;
	}

	bool TextureDecode::Decode(const std::string& path, DecodedImage& out)
	{
		return DecodeNow(path, out);
	}

	void TextureDecode::Clear()
	{
		std::lock_guard lock(s_SlotsMutex);
		s_Slots.clear();
	}
}
