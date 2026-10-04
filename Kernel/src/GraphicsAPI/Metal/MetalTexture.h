#pragma once

#ifdef PLATFORM_MACOS

#include "Helios/VirtualDevice/DeviceTexture.h"
#include <Metal/Metal.hpp>

namespace Helios
{
    /* Metal 纹理实现。
     * 纹理类型（2D / 2DArray / Cube / 3D）、采样器与 mip 层级由
     * TextureDesc / TextureLoadConfig 推导；像素格式转换统一走 MetalConversions。 */
    class MetalTexture : public DeviceTexture
    {
    public:
        MetalTexture(const std::string& name, const TextureDesc& texture_desc);
        MetalTexture(const std::string& path, const TextureLoadConfig& load_config);
        ~MetalTexture() override;

        /* 绑定纹理与配套采样器到指定槽位（顶点/片元阶段同时可见）。
         * 槽位即 GLSL 中的 binding，与编译期生成的 [[texture(N)]] / [[sampler(N)]] 一致。 */
        void Bind(uint32_t slot = 0) override;
        void Unbind() override;
        void GenerateMipmap() override;

        void SetData(void* data, const PixelDesc& pixel_desc, uint32_t level = 0,
            uint32_t offset_x = 0, uint32_t offset_y = 0, uint32_t offset_z = 0) override;

        /* 回读某一 mip / 面（layer）的原始 texel 数据。纹理为私有存储时不可读。 */
        bool ReadbackPixels(std::vector<uint8_t>& out_data, uint32_t mip_level = 0, uint32_t layer = 0) override;

        [[nodiscard]] const std::string& GetPath() const override { return m_Path; }

        /* 硬件句柄。注意：受接口返回类型限制会被截断为 32 位，
         * 仅用于调试/兼容，不作为纹理的跨模块标识使用。 */
        [[nodiscard]] uint32_t GetTextureID() const override
        {
            return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m_Texture));
        }

        [[nodiscard]] bool IsLoaded() const override { return m_IsLoaded; }

        bool operator==(const DeviceTexture& other) const override;

        /* Metal 特有接口 */
        MTL::Texture* GetMetalTexture() const { return m_Texture; }
        MTL::SamplerState* GetSamplerState() const { return m_SamplerState; }

        /* 指定层级/切片是否可作渲染目标（供 FrameBuffer 校验） */
        [[nodiscard]] bool IsRenderTarget() const;

    private:
        /* 按 TextureDesc 创建设备纹理（渲染目标与已加载数据共用） */
        void CreateTexture(const TextureDesc& desc);
        /* 按加载配置创建采样器状态；渲染目标使用默认采样器 */
        void CreateSamplerState(const TextureLoadConfig& load_config);
        void LoadFromFile(const std::string& path, const TextureLoadConfig& load_config);

        /* 依据尺寸与是否生成 mip 推导 mip 层级数 */
        static uint8_t ResolveMipLevels(uint32_t width, uint32_t height, bool gen_mips);

        MTL::Texture* m_Texture{ nullptr };
        MTL::SamplerState* m_SamplerState{ nullptr };
        std::string m_Path;
        bool m_IsLoaded{ false };
    };
}

#endif /* PLATFORM_MACOS */
