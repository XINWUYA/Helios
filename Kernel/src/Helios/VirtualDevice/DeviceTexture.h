#pragma once
#include <cstdint>
#include <vector>
#include <Helios/Renderer/RenderCommon.h>

namespace Helios
{
	/* 纹理信息描述 */
	struct TextureDesc
	{
		uint32_t Width{ 1 };								/* 宽度 */
		uint32_t Height{ 1 };								/* 高度 */
		uint32_t Depth{ 1 };								/* 深度 */
		uint8_t MipLevels{ 1 };								/* Mip层级 */
		uint8_t Samples{ 0 };								/* 采样次数 */
		TextureFormat Format{ TextureFormat::RGBA8 };		/* 像素格式：RxGxBxAx等 */
		SamplerType SamplerType{ SamplerType::Sampler2D };	/* 采样方式： Sampler2D/2DArray/CubeMap/3D等 */
		TextureUsage Usage{ TextureUsage::Sampleable };		/* 使用方式：ColorAttachment/DepthAttachment/StencilAttachment/Sampleable等 */
	};

	/* 像素数据描述 */
	struct PixelDesc
	{
		PixelFormat Format{ PixelFormat::RGBA };
		PixelType Type{ PixelType::UnsignedByte };
	};

	/* 纹理加载配置 */
	struct TextureLoadConfig
	{
		bool IsFlipV = true;
		bool IsGenMips = true;
		bool IsHdr = false;
		bool IsSrgb = false;
		SamplerType SamplerType{ SamplerType::Sampler2D }; /* 用于决定纹理加载的类型 */
		SamplerWrapMode SamplerWrapMode{ SamplerWrapMode::Repeat };
		SamplerMinFilter SamplerMinFilter{ SamplerMinFilter::Linear };
		SamplerMagFilter SamplerMagFilter{ SamplerMagFilter::Linear };

		bool operator==(const TextureLoadConfig& load_config) const
		{
			return IsFlipV == load_config.IsFlipV
				&& IsGenMips == load_config.IsGenMips
				&& IsHdr == load_config.IsHdr
				&& IsSrgb == load_config.IsSrgb
				&& SamplerType == load_config.SamplerType
				&& SamplerWrapMode == load_config.SamplerWrapMode
				&& SamplerMinFilter == load_config.SamplerMinFilter
				&& SamplerMagFilter == load_config.SamplerMagFilter;
		}

		bool operator!=(const TextureLoadConfig& load_config) const
		{
			return !(*this == load_config);
		}
	};


	/* 通用纹理类，根据描述和用途创建不同类型的纹理 */
	class DeviceTexture
	{
	public:
		DeviceTexture(std::string name, const TextureDesc& texture_desc);
		virtual ~DeviceTexture() = default;

		/* 绑定 */
		virtual void Bind(uint32_t slot = 0) = 0;
		virtual void Unbind() = 0;

		/* 生成 mipmap（驱动端 glGenerateMipmap，从 mip 0 自动填充所有低层级） */
		virtual void GenerateMipmap() {}

		/* 填充数据 */
		virtual void SetData(void* data, const PixelDesc& pixel_desc, uint32_t level = 0,
			uint32_t offset_x = 0, uint32_t offset_y = 0, uint32_t offset_z = 0) = 0;

		/* 把指定 mip / 层的像素读回 CPU（层对立方图来说就是面索引）。数据是纹理自身格式的原始
		 * texel 字节。只保证"拷出此刻内容"：绘制还没执行完就读到旧数据，得先 WaitForGPU()。
		 * 后端不支持或纹理私有存储时返回 false。 */
		virtual bool ReadbackPixels(std::vector<uint8_t>& /*out_data*/, uint32_t /*mip_level*/ = 0, uint32_t /*layer*/ = 0)
		{
			return false;
		}

		/* 获取名称 */
		[[nodiscard]] const std::string& GetDebugName() const { return m_DebugName; }
		/* 获取纹理原始尺寸 */
		[[nodiscard]] uint32_t GetWidth() const { return m_TextureDesc.Width; }
		[[nodiscard]] uint32_t GetHeight() const { return m_TextureDesc.Height; }
        /* 获取纹理信息描述 */
        [[nodiscard]] const TextureDesc& GetTextureDesc() const { return m_TextureDesc; }
		/* 获取纹理加载配置 */
		[[nodiscard]] const TextureLoadConfig& GetTextureLoadConfig() const { return m_TextureLoadConfig; }

		/* 获取纹理所在路径 */
		[[nodiscard]] virtual const std::string& GetPath() const = 0;
		/* 纹理ID */
		[[nodiscard]] virtual uint32_t GetTextureID() const = 0;
		/* 纹理成功加载 */
		[[nodiscard]] virtual bool IsLoaded() const = 0;

		/* 重载operators */
		virtual bool operator==(const DeviceTexture& other) const = 0;

		/* 创建纹理 */
		static SharedPtr<DeviceTexture> Create(const std::string& name, const TextureDesc& texture_desc);
		static SharedPtr<DeviceTexture> Create(const std::string& path, const TextureLoadConfig& load_config = {}); /* 目前仅支持加载二维纹理 */
		static SharedPtr<DeviceTexture> CreateWithSolidColor(const glm::vec3& color); /* 创建一个纯色贴图 */

		/* 默认纹理 */
		static SharedPtr<DeviceTexture> White();
		static SharedPtr<DeviceTexture> Black();
		static SharedPtr<DeviceTexture> Normal();
		/* 默认立方图（1x1、六面全黑）：给"暂不使用"的立方采样槽位做中性绑定。
		 * Metal 要求片元函数声明的采样器在每次绘制都全部有绑定 —— u_UseIBL=0
		 * 之类的门控只关取值、不能省绑定（缺一即被校验层断言，见 IBL.glsl）。 */
		static SharedPtr<DeviceTexture> BlackCube();

	protected:
		DeviceTexture() = default;

		/* 名称 */
		std::string m_DebugName{ "Unnamed DeviceTexture" };
		/* 纹理描述 */
		TextureDesc m_TextureDesc{};
		/* 加载配置 */
		TextureLoadConfig m_TextureLoadConfig{};
	};
}


