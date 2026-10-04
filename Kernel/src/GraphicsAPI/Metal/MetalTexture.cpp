#ifdef PLATFORM_MACOS

#include "Pch.h"
#include "MetalTexture.h"
#include "MetalCommon.h"
#include "MetalConversions.h"
#include "stb_image.h"

#include <algorithm>
#include <cmath>

namespace Helios
{
    namespace
    {
        /* 逐 mip 层级的尺寸（每次减半，最小为 1） */
        [[nodiscard]] uint32_t MipDimension(uint32_t size, uint32_t level) noexcept
        {
            return std::max(1u, size >> level);
        }
    }

    MetalTexture::MetalTexture(const std::string& name, const TextureDesc& texture_desc)
        : DeviceTexture(name, texture_desc)
    {
        PROFILE_FUNCTION();

        CreateTexture(texture_desc);
        CreateSamplerState(TextureLoadConfig{});
        m_IsLoaded = m_Texture != nullptr;
    }

    MetalTexture::MetalTexture(const std::string& path, const TextureLoadConfig& load_config)
        : DeviceTexture(path, TextureDesc{})
    {
        PROFILE_FUNCTION();

        LoadFromFile(path, load_config);
    }

    MetalTexture::~MetalTexture()
    {
        PROFILE_FUNCTION();

        if (m_Texture)
        {
            m_Texture->release();
            m_Texture = nullptr;
        }

        if (m_SamplerState)
        {
            m_SamplerState->release();
            m_SamplerState = nullptr;
        }
    }

    void MetalTexture::Bind(uint32_t slot)
    {
        PROFILE_FUNCTION();

        MTL::RenderCommandEncoder* encoder = MetalRuntime::Encoder();
        if (!encoder)
            return;

        /* 顶点与片元阶段都绑定：顶点纹理拾取（如高程采样）同样需要 */
        if (m_Texture)
        {
            encoder->setVertexTexture(m_Texture, slot);
            encoder->setFragmentTexture(m_Texture, slot);
        }

        /* 采样器与纹理使用同一槽位：ShaderCompiler 生成 MSL 时为 combined
         * image sampler 同时输出 [[texture(N)]] 与 [[sampler(N)]]，二者索引相同。 */
        if (m_SamplerState)
        {
            encoder->setVertexSamplerState(m_SamplerState, slot);
            encoder->setFragmentSamplerState(m_SamplerState, slot);
        }
    }

    void MetalTexture::Unbind()
    {
        /* Metal 的纹理绑定随编码器生命周期结束而失效，无需显式解绑 */
    }

    void MetalTexture::GenerateMipmap()
    {
        PROFILE_FUNCTION();

        if (!m_Texture || m_Texture->mipmapLevelCount() <= 1)
            return;

        MTL::CommandQueue* queue = MetalRuntime::Queue();
        if (!queue)
        {
            CORE_LOG_ERROR("MetalTexture::GenerateMipmap: command queue unavailable");
            return;
        }

        MTL::CommandBuffer* command_buffer = queue->commandBuffer();
        if (!command_buffer)
            return;

        MTL::BlitCommandEncoder* blit_encoder = command_buffer->blitCommandEncoder();
        if (!blit_encoder)
        {
            command_buffer->release();
            return;
        }

        blit_encoder->generateMipmaps(m_Texture);
        blit_encoder->endEncoding();
        command_buffer->commit();

        /* 独立命令缓冲区提交后同步等待：mip 生成必须在本帧采样之前完成。
         * 编码器与命令缓冲区由 Metal 自动释放池管理，此处不手动 release。 */
        command_buffer->waitUntilCompleted();
    }

    void MetalTexture::SetData(void* data, const PixelDesc& pixel_desc, uint32_t level,
        uint32_t offset_x, uint32_t offset_y, uint32_t offset_z)
    {
        PROFILE_FUNCTION();

        if (!m_Texture || !data)
            return;

        if (level >= m_Texture->mipmapLevelCount())
        {
            CORE_LOG_ERROR("MetalTexture::SetData: mip level {} out of range ({})",
                level, m_Texture->mipmapLevelCount());
            return;
        }

        if (m_TextureDesc.Samples > 1)
        {
            CORE_LOG_ERROR("MetalTexture::SetData: multisample textures cannot be uploaded from CPU");
            return;
        }

        const uint32_t bytes_per_pixel = GetPixelDescBytesPerPixel(pixel_desc);
        if (bytes_per_pixel == 0)
        {
            CORE_LOG_ERROR("MetalTexture::SetData: unsupported pixel desc for {}", m_Path);
            return;
        }

        const uint32_t level_width = MipDimension(m_TextureDesc.Width, level);
        const uint32_t level_height = MipDimension(m_TextureDesc.Height, level);
        const uint32_t level_depth = MipDimension(std::max(1u, m_TextureDesc.Depth), level);

        /* 区域起点由调用方指定，尺寸取该层级剩余部分：
         * 上传整层时 offset 为 0，局部更新时只覆盖对应矩形。 */
        const uint32_t region_width = level_width > offset_x ? level_width - offset_x : 0;
        const uint32_t region_height = level_height > offset_y ? level_height - offset_y : 0;
        if (region_width == 0 || region_height == 0)
        {
            CORE_LOG_ERROR("MetalTexture::SetData: empty region at level {} ({}, {})", level, offset_x, offset_y);
            return;
        }

        const MTL::TextureType texture_type = m_Texture->textureType();
        const bool is_3d = texture_type == MTL::TextureType3D;
        const uint32_t region_depth = is_3d
            ? (level_depth > offset_z ? level_depth - offset_z : 0)
            : 1;

        if (region_depth == 0)
            return;

        MTL::Region region;
        region.origin.x = offset_x;
        region.origin.y = offset_y;
        region.origin.z = is_3d ? offset_z : 0;
        region.size.width = region_width;
        region.size.height = region_height;
        region.size.depth = region_depth;

        const NS::UInteger bytes_per_row = region_width * bytes_per_pixel;
        const NS::UInteger bytes_per_image = bytes_per_row * region_height;

        if (is_3d)
        {
            /* 3D 纹理：一次可覆盖多个切片，需要提供 slice 间距 */
            m_Texture->replaceRegion(region, level, 0, data, bytes_per_row, bytes_per_image);
        }
        else
        {
            /* 2D / 2DArray / Cube：slice 参数依次表示切片索引、立方图的 face 索引 */
            m_Texture->replaceRegion(region, level, offset_z, data, bytes_per_row, 0);
        }
    }

    bool MetalTexture::operator==(const DeviceTexture& other) const
    {
        const auto* metal_other = dynamic_cast<const MetalTexture*>(&other);
        return metal_other != nullptr && metal_other->m_Texture == m_Texture;
    }

    bool MetalTexture::IsRenderTarget() const
    {
        return m_Texture && (m_Texture->usage() & MTL::TextureUsageRenderTarget) != 0;
    }

    void MetalTexture::CreateTexture(const TextureDesc& desc)
    {
        MTL::Device* device = MetalRuntime::Device();
        if (!device)
        {
            CORE_LOG_ERROR("MetalTexture: Metal device unavailable, cannot create '{}'", m_DebugName);
            return;
        }

        const MTL::PixelFormat pixel_format = ToMetalPixelFormat(desc.Format);
        if (pixel_format == MTL::PixelFormatInvalid)
        {
            CORE_LOG_ERROR("MetalTexture: invalid pixel format for '{}'", m_DebugName);
            return;
        }

        /* 采样数 > 1 时使用多重采样纹理；Cube/3D 不支持 MSAA，降级为 1 并提示。 */
        uint32_t samples = desc.Samples;
        MTL::TextureType texture_type = ToMetalSamplerType(desc.SamplerType);

        if (samples > 1)
        {
            if (desc.SamplerType == SamplerType::Sampler2D)
            {
                texture_type = MTL::TextureType2DMultisample;
            }
            else if (desc.SamplerType == SamplerType::Sampler2DArray)
            {
                texture_type = MTL::TextureType2DMultisampleArray;
            }
            else
            {
                CORE_LOG_WARN("MetalTexture: MSAA requested for non-2D sampler type on '{}', using 1 sample",
                    m_DebugName);
                samples = 1;
            }
        }

        const bool multisample = samples > 1;
        const uint32_t width = std::max(1u, desc.Width);
        const uint32_t height = std::max(1u, desc.Height);

        /* 多重采样纹理不能有 mip 层级，也不能由 CPU 上传 */
        const uint32_t mip_levels = multisample ? 1u : std::max<uint32_t>(1u, desc.MipLevels);

        MTL::TextureDescriptor* texture_desc = MTL::TextureDescriptor::alloc()->init();
        texture_desc->setTextureType(texture_type);
        texture_desc->setPixelFormat(pixel_format);
        texture_desc->setWidth(width);
        texture_desc->setHeight(height);
        texture_desc->setMipmapLevelCount(mip_levels);
        /* MetalStorage::TextureOptions 同时限定了存储模式，此处取其存储模式部分 */
        texture_desc->setStorageMode(static_cast<MTL::StorageMode>(MetalStorage::TextureOptions));

        if (multisample)
            texture_desc->setSampleCount(samples);

        switch (desc.SamplerType)
        {
        case SamplerType::Sampler2DArray:
            /* Depth 表示数组层数，采样数 > 1 时对应多重采样数组 */
            texture_desc->setArrayLength(std::max(1u, desc.Depth));
            break;
        case SamplerType::SamplerCubeMap:
            /* 立方图的 6 个面是 Metal 隐式定义的 slice(0~5)，arrayLength 必须保持 1：
             * 设成 6 会被描述符校验直接拒绝（"MTLTextureTypeCube requires that
             * arrayLength is 1"）。逐面渲染由 FrameBuffer 附件的 setSlice(face) 指定。 */
            break;
        case SamplerType::Sampler3D:
            texture_desc->setDepth(std::max(1u, desc.Depth));
            break;
        case SamplerType::Sampler2D:
        default:
            break;
        }

        /* 渲染目标与可采样用途按描述组合；上传路径只要求 CPU 可写（由存储模式保证） */
        MTL::TextureUsage usage = MTL::TextureUsageShaderRead;
        if (HasTextureUsage(desc.Usage, TextureUsage::ColorAttachment) ||
            HasTextureUsage(desc.Usage, TextureUsage::DepthAttachment) ||
            HasTextureUsage(desc.Usage, TextureUsage::StencilAttachment))
        {
            usage |= MTL::TextureUsageRenderTarget;
        }
        texture_desc->setUsage(usage);

        m_Texture = device->newTexture(texture_desc);
        texture_desc->release();

        if (!m_Texture)
        {
            CORE_LOG_ERROR("MetalTexture: failed to create texture '{}' ({}x{}, format {})",
                m_DebugName, width, height, static_cast<int>(desc.Format));
        }
    }

    void MetalTexture::CreateSamplerState(const TextureLoadConfig& load_config)
    {
        MTL::Device* device = MetalRuntime::Device();
        if (!device)
        {
            CORE_LOG_ERROR("MetalTexture: Metal device unavailable, cannot create sampler for '{}'", m_DebugName);
            return;
        }

        MTL::SamplerDescriptor* sampler_desc = MTL::SamplerDescriptor::alloc()->init();
        sampler_desc->setMinFilter(ToMetalSamplerMinMagFilter(load_config.SamplerMinFilter));
        sampler_desc->setMagFilter(ToMetalSamplerMagFilter(load_config.SamplerMagFilter));

        /* 配置不做 mip 过滤但纹理存在多个层级时启用线性 mip 过滤，
         * 否则生成的 mip 层级不会被采样到。 */
        MTL::SamplerMipFilter mip_filter = ToMetalSamplerMipFilter(load_config.SamplerMinFilter);
        if (mip_filter == MTL::SamplerMipFilterNotMipmapped && m_TextureDesc.MipLevels > 1)
            mip_filter = MTL::SamplerMipFilterLinear;
        sampler_desc->setMipFilter(mip_filter);

        const MTL::SamplerAddressMode address_mode = ToMetalSamplerAddressMode(load_config.SamplerWrapMode);
        sampler_desc->setSAddressMode(address_mode);
        sampler_desc->setTAddressMode(address_mode);
        sampler_desc->setRAddressMode(address_mode);

        /* 各向异性过滤：对斜视的大面积纹理收益明显，开销可忽略 */
        sampler_desc->setMaxAnisotropy(8);
        sampler_desc->setNormalizedCoordinates(true);
        sampler_desc->setLodMinClamp(0.0f);

        m_SamplerState = device->newSamplerState(sampler_desc);
        sampler_desc->release();

        if (!m_SamplerState)
        {
            CORE_LOG_ERROR("MetalTexture: failed to create sampler state for '{}'", m_DebugName);
        }
    }

    void MetalTexture::LoadFromFile(const std::string& path, const TextureLoadConfig& load_config)
    {
        PROFILE_FUNCTION();

        m_Path = path;
        m_TextureLoadConfig = load_config;

        stbi_set_flip_vertically_on_load(load_config.IsFlipV ? 1 : 0);

        int width = 0;
        int height = 0;
        int channels = 0;
        void* data = nullptr;
        PixelDesc pixel_desc{};

        if (load_config.IsHdr)
        {
            /* HDR 数据以 float 加载，使用 32 位浮点格式原样上传 */
            data = stbi_loadf(path.c_str(), &width, &height, &channels, 4);
            pixel_desc = PixelDesc{ PixelFormat::RGBA, PixelType::Float };
            m_TextureDesc.Format = TextureFormat::RGBA32F;
        }
        else
        {
            data = stbi_load(path.c_str(), &width, &height, &channels, 4);
            pixel_desc = PixelDesc{ PixelFormat::RGBA, PixelType::UnsignedByte };
            /* 颜色纹理使用 sRGB 格式，采样时由硬件完成线性化 */
            m_TextureDesc.Format = load_config.IsSrgb
                ? ToSrgbFormat(TextureFormat::RGBA8)
                : TextureFormat::RGBA8;
        }

        if (!data)
        {
            CORE_LOG_ERROR("MetalTexture: failed to load texture '{}'", path);
            m_IsLoaded = false;
            return;
        }

        m_TextureDesc.Width = static_cast<uint32_t>(width);
        m_TextureDesc.Height = static_cast<uint32_t>(height);
        m_TextureDesc.Depth = 1;
        m_TextureDesc.SamplerType = load_config.SamplerType;
        m_TextureDesc.Usage = TextureUsage::Sampleable;
        m_TextureDesc.Samples = 1;
        m_TextureDesc.MipLevels = ResolveMipLevels(m_TextureDesc.Width, m_TextureDesc.Height, load_config.IsGenMips);

        CreateTexture(m_TextureDesc);
        CreateSamplerState(load_config);

        if (!m_Texture)
        {
            stbi_image_free(data);
            m_IsLoaded = false;
            return;
        }

        SetData(data, pixel_desc, 0);
        stbi_image_free(data);

        if (m_TextureDesc.MipLevels > 1)
            GenerateMipmap();

        m_IsLoaded = true;
        CORE_LOG_INFO("Texture loaded: {} ({}x{}, {} mip levels, {} bytes/pixel)",
            path, width, height, m_TextureDesc.MipLevels, GetPixelDescBytesPerPixel(pixel_desc));
    }

    uint8_t MetalTexture::ResolveMipLevels(uint32_t width, uint32_t height, bool gen_mips)
    {
        if (!gen_mips)
            return 1;

        const uint32_t max_dimension = std::max(width, height);
        if (max_dimension <= 1)
            return 1;

        return static_cast<uint8_t>(std::floor(std::log2(static_cast<float>(max_dimension)))) + 1;
    }
}

#endif /* PLATFORM_MACOS */
