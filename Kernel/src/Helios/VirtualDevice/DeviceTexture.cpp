#include "Pch.h"
#include "DeviceTexture.h"
#include "Helios/Core/TextureSettings.h"
#include "Helios/Renderer/Renderer.h"
#include "GraphicsAPI/OpenGL/OpenGLTexture.h"
#ifdef PLATFORM_MACOS
#include "GraphicsAPI/Metal/MetalTexture.h"
#endif

namespace Helios
{
	uint32_t TextureLoadConfig::Fingerprint() const
	{
		/* FNV-1a：字段少，直接按字节混，够用且不依赖 std::hash 的实现 */
		uint32_t hash = 2166136261u;
		const auto mix = [&hash](const void* data, size_t size)
		{
			const auto* bytes = static_cast<const uint8_t*>(data);
			for (size_t index = 0; index < size; ++index)
			{
				hash ^= bytes[index];
				hash *= 16777619u;
			}
		};

		mix(&IsFlipV, sizeof(IsFlipV));
		mix(&IsGenMips, sizeof(IsGenMips));
		mix(&IsHdr, sizeof(IsHdr));
		mix(&IsSrgb, sizeof(IsSrgb));
		mix(&SamplerType, sizeof(SamplerType));
		mix(&SamplerWrapMode, sizeof(SamplerWrapMode));
		mix(&SamplerMinFilter, sizeof(SamplerMinFilter));
		mix(&SamplerMagFilter, sizeof(SamplerMagFilter));
		mix(&AnisoLevel, sizeof(AnisoLevel));
		return hash;
	}

	DeviceTexture::DeviceTexture(std::string name, const TextureDesc& texture_desc)
		: m_DebugName(std::move(name)), m_TextureDesc(texture_desc)
	{
	}

	/* 创建纹理 */
	SharedPtr<DeviceTexture> DeviceTexture::Create(const std::string& name, const TextureDesc& texture_desc)
	{
		switch (Renderer::CurrentAPI())
		{
		case RenderAPI::None:
			CORE_LOG_ERROR("RenderAPI can't be None!");
			return nullptr;
		case RenderAPI::OpenGL:
			return CreateSharedPtr<OpenGLTexture>(name, texture_desc);
#ifdef PLATFORM_MACOS
		case RenderAPI::Metal:
			return CreateSharedPtr<MetalTexture>(name, texture_desc);
#endif
		default:
			CORE_LOG_ERROR("Unknown RenderAPI is unsupported!");
			return nullptr;
		}
	}

	/* 创建纹理 */
	SharedPtr<DeviceTexture> DeviceTexture::Create(const std::string& path, const TextureLoadConfig& load_config)
	{
		/* 资产设置在这里统一落地：材质、精灵、脚本、缩略图走的都是同一条路 */
		const TextureLoadConfig resolved = TextureSettingsStore::Instance().Resolve(path, load_config);

		SharedPtr<DeviceTexture> texture;
		switch (Renderer::CurrentAPI())
		{
		case RenderAPI::None:
			CORE_LOG_ERROR("RenderAPI can't be None!");
			return nullptr;
		case RenderAPI::OpenGL:
			texture = CreateSharedPtr<OpenGLTexture>(path, resolved);
			break;
#ifdef PLATFORM_MACOS
		case RenderAPI::Metal:
			texture = CreateSharedPtr<MetalTexture>(path, resolved);
			break;
#endif
		default:
			CORE_LOG_ERROR("Unknown RenderAPI is unsupported!");
			return nullptr;
		}

		/* 记下调用点原始配置：资产设置改了以后，Reload 要按它重新解析 */
		if (texture != nullptr)
			texture->m_RequestedLoadConfig = load_config;
		return texture;
	}

	/* 创建一个纯色贴图 */
	SharedPtr<DeviceTexture> DeviceTexture::CreateWithSolidColor(const glm::vec3& color)
	{
		TextureDesc desc;
		auto texture = DeviceTexture::Create("SolidTexture", desc);
		const uint8_t pixel[4] = {
			static_cast<uint8_t>(glm::clamp(color.r, 0.0f, 1.0f) * 255.0f),
			static_cast<uint8_t>(glm::clamp(color.g, 0.0f, 1.0f) * 255.0f),
			static_cast<uint8_t>(glm::clamp(color.b, 0.0f, 1.0f) * 255.0f),
			255
		};

		PixelDesc pixel_desc;
		pixel_desc.Format = PixelFormat::RGBA;
		pixel_desc.Type = PixelType::UnsignedByte;
		texture->SetData(const_cast<uint8_t*>(pixel), pixel_desc);
		return texture;
	}

	/* 默认纹理 */
	SharedPtr<DeviceTexture> DeviceTexture::White()
	{
		static SharedPtr<DeviceTexture> texture;

		if (!texture)
		{
			// DeviceTexture
			constexpr TextureDesc desc{ 2,2 };
			texture = DeviceTexture::Create("DefaultWhiteTex", desc);
			uint32_t default_texture_data[4] = {
				0xffffffff,
				0xffffffff,
				0xffffffff,
				0xffffffff,
			}; // White
			texture->SetData(&default_texture_data, {});
		}

		return texture;
	}

	SharedPtr<DeviceTexture> DeviceTexture::Black()
	{
		static SharedPtr<DeviceTexture> texture;

		if (!texture)
		{
			// DeviceTexture
			constexpr TextureDesc desc{ 2,2 };
			texture = DeviceTexture::Create("DefaultBlackTex", desc);
			uint32_t default_texture_data[4] = {
				0x00000000,
				0x00000000,
				0x00000000,
				0x00000000,
			}; // Black
			texture->SetData(default_texture_data, {});
		}

		return texture;
	}

	SharedPtr<DeviceTexture> DeviceTexture::Normal()
	{
		static SharedPtr<DeviceTexture> texture;

		if (!texture)
		{
			// DeviceTexture
			constexpr TextureDesc desc{ 2,2 };
			texture = DeviceTexture::Create("DefaultNormalTex", desc);
			/* 平面法线 = (128,128,255)：采样后经 *2-1 解码为 (0,0,1)（切空间朝上、不加扰动）。
			 * 不能存 (0,0,255)——那是"原始蓝"约定，在本引擎（消费方一律 *2-1）解出 (-1,-1,1)，
			 * TBN 后是倾斜 55° 的错法线（地面等缺省补图材质会整片明暗失真）。 */
			uint32_t default_texture_data[4] = {
				0xffff8080,
				0xffff8080,
				0xffff8080,
				0xffff8080,
			}; // Flat normal (128,128,255)
			texture->SetData(default_texture_data, {});
		}

		return texture;
	}

	SharedPtr<DeviceTexture> DeviceTexture::BlackCube()
	{
		/* 单实例：每帧可能被多处绑定（无 IBL 时的中性兜底），不能每次调用都新建上传 */
		static SharedPtr<DeviceTexture> texture;

		if (!texture)
		{
			TextureDesc desc;
			desc.SamplerType = SamplerType::SamplerCubeMap;
			desc.Width = 1;
			desc.Height = 1;
			desc.MipLevels = 1;
			texture = DeviceTexture::Create("DefaultBlackCube", desc);

			/* 立方图逐面上传（offset_z = 面索引） */
			uint32_t black = 0x00000000;
			for (uint32_t face = 0; face < 6; ++face)
				texture->SetData(&black, {}, 0, 0, 0, face);
		}

		return texture;
	}
}
