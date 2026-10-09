#include "Pch.h"
#include "ReflectionProbe.h"
#include "ReflectionProbeBakeCache.h"
#include "SceneCommon.h"
#include "Helios/Renderer/FrameGraph/FrameGraph.h"
#include <Helios/Renderer/RenderView.h>
#include <Helios/Renderer/Renderer.h>
#include <Helios/Renderer/RenderCommon.h>
#include <Helios/Common/Math.h>
#include <Helios/Scene/Camera.h>
#include <Helios/Scene/Mesh.h>
#include <Helios/Scene/Material.h>
#include <Helios/VirtualDevice/DeviceTexture.h>
#include <Helios/VirtualDevice/DeviceShader.h>
#include <Helios/VirtualDevice/DeviceFrameBuffer.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <unordered_set>
#include <vector>


namespace Helios
{
    ReflectionProbe::ReflectionProbe()
    {
        m_ObjectType = ObjectType::ReflectionProbe;
    }

    void ReflectionProbe::Bake(RenderView* render_view)
    {
        PROFILE_FUNCTION();

        const bool need_bake = m_IsRealtime || !m_BakeCompleted && (m_BakeConfig.BakeDiffuse || m_BakeConfig.BakeSpecular);
        if (!need_bake)
            return;

        /* 烘焙场景到立方体贴图 */
        BakeEnvCubemap(render_view);
        if (!m_BakeResult.EnvColorCubemap)
            return;

        /* 立方图只渲染了 mip0：卷积需要采样更模糊的层级来近似"按立体角积分"，否则太阳这类极亮的
         * 小特征会被点采样成孤立超亮 texel。要用 RenderAPI 的版本（记录进当前帧命令流）——
         * GenerateMipmap 独立提交命令缓冲区会先执行、mip 全是 0。 */
        Renderer::GetRenderAPI()->GenerateMipmap(m_BakeResult.EnvColorCubemap);

        if (m_BakeConfig.BakeDiffuse)
            BakeIrradianceMap();

        if (m_BakeConfig.BakeSpecular)
            BakePrefilterMap();

        m_BakeCompleted = true;
    }

    void ReflectionProbe::Reset()
    {
        m_BakeResult = BakeResult{};
        m_BakeCompleted = false;

        /* 烘焙结果作废，预览随之作废：清空后由 UI 的按需请求在重新烘焙完成后重建 */
        for (auto& preview : m_BakePreviews)
            preview = nullptr;
        m_PreviewState = BakePreviewState::Idle;
        m_PreviewFailed = false;
    }

    void ReflectionProbe::RequestRebake()
    {
        m_RebakeRequested = true;
    }

    /* ==================== 烘焙结果缓存 ==================== */

    namespace
    {
        /* 烘焙完成后至少等待的帧数：让承载烘焙绘制的命令缓冲区先提交，
         * 随后 TickBakeCacheWrite 才能在它执行完成后回读。 */
        constexpr uint8_t kBakeCacheWriteFrameDelay = 1;

        /* 缓存里用到的纹理格式 -> 上传时需要的数据描述 */
        bool ResolveBakeCachePixelDesc(TextureFormat format, PixelDesc& out_desc)
        {
            switch (format)
            {
            case TextureFormat::RGBA16F: out_desc = PixelDesc{ PixelFormat::RGBA, PixelType::Half };         return true;
            case TextureFormat::RGBA32F: out_desc = PixelDesc{ PixelFormat::RGBA, PixelType::Float };        return true;
            case TextureFormat::RGBA8:   out_desc = PixelDesc{ PixelFormat::RGBA, PixelType::UnsignedByte }; return true;
            default:                     return false;
            }
        }

        /* ==================== 烘焙结果预览（十字展开图） ==================== */

        /* 预览里每个面的目标边长：取到 64 就够看清结构（从 mip 链里取，不回读全尺寸） */
        constexpr uint32_t kBakePreviewFaceSize = 64;

        /* 预览生成在烘焙完成后至少等待的帧数：让承载烘焙绘制的命令缓冲区先提交 */
        constexpr uint8_t kBakePreviewFrameDelay = 1;

        /* IEEE 754 半精度 -> float。回读的 RGBA16F 是原始字节，CPU 侧要自己解出数值；
         * 不用平台的 _Float16 —— 跨编译器（MSVC / clang）行为一致更重要。 */
        float HalfToFloat(uint16_t half)
        {
            const uint32_t sign = static_cast<uint32_t>(half & 0x8000u) << 16;
            const uint32_t exponent = (half >> 10) & 0x1Fu;
            uint32_t mantissa = half & 0x03FFu;

            uint32_t bits = 0;
            if (exponent == 0)
            {
                if (mantissa == 0)
                {
                    bits = sign; /* ±0 */
                }
                else
                {
                    /* 非规格化数：左移规范化，直到隐含位进入 mantissa 的 bit10 */
                    int32_t shift = -1;
                    do
                    {
                        ++shift;
                        mantissa <<= 1;
                    } while ((mantissa & 0x400u) == 0);
                    mantissa &= 0x03FFu;
                    bits = sign | (static_cast<uint32_t>(112 - shift) << 23) | (mantissa << 13);
                }
            }
            else if (exponent == 0x1Fu)
            {
                bits = sign | 0x7F800000u | (mantissa << 13); /* Inf / NaN */
            }
            else
            {
                bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
            }

            float result = 0.0f;
            std::memcpy(&result, &bits, sizeof(float));
            return result;
        }

        /* HDR 值 -> 8 位显示值：Reinhard 压缩 + sRGB 近似 gamma。
         * 预览是"看结构"用的，不做精确色彩管理。 */
        uint8_t TonemapToByte(float value)
        {
            value = std::max(value, 0.0f);
            const float mapped = value / (1.0f + value);
            return static_cast<uint8_t>(std::pow(mapped, 1.0f / 2.2f) * 255.0f + 0.5f);
        }

        /* 十字展开布局（4 列 x 3 行）里每个面的格子坐标；面序 = +X,-X,+Y,-Y,+Z,-Z（与捕获相机一致） */
        constexpr uint32_t kCrossColumn[6] = { 2, 0, 1, 1, 1, 3 };
        constexpr uint32_t kCrossRow[6] = { 1, 1, 0, 2, 1, 1 };

        /* 预览 mip 的选取结果：往低分辨率走，直到封面边长不超过目标（不能越过可用 mip 链）。
         * GPU 回读路径与公共构建（BuildPreviewTexture）共用同一套选取。 */
        struct PreviewMipSelection
        {
            uint32_t Mip{ 0 };
            uint32_t FaceSize{ 1 };
        };

        PreviewMipSelection SelectPreviewMip(uint32_t mip0_size, uint32_t mip_count)
        {
            PreviewMipSelection selection{ 0, std::max(mip0_size, 1u) };
            while (selection.FaceSize > kBakePreviewFaceSize && selection.Mip + 1 < std::max(mip_count, 1u))
            {
                selection.FaceSize = std::max(selection.FaceSize >> 1, 1u);
                ++selection.Mip;
            }

            return selection;
        }

        /* GPU 侧：从设备立方图回读预览用的那个 mip，组成一张"单 mip"的缓存图，
         * 其余交给公共的 BuildPreviewTexture —— 与"读 .probe 文件"走同一条转换路径。
         * 只回读一个 mip（封面到 ~64px），不搬全尺寸数据。 */
        SharedPtr<DeviceTexture> BuildGpuBakePreviewTexture(const std::string& name, const SharedPtr<DeviceTexture>& source)
        {
            if (source == nullptr)
                return nullptr;

            const TextureDesc& source_desc = source->GetTextureDesc();
            const PreviewMipSelection selection = SelectPreviewMip(source_desc.Width,
                std::max<uint32_t>(1u, source_desc.MipLevels));

            ReflectionProbeBakeCache::Image image;
            image.Format = source_desc.Format;
            image.Size = selection.FaceSize;    /* 这张"图"就是选中的那个 mip */
            image.Mips.resize(1);
            image.Mips[0].resize(6);

            for (uint32_t face = 0; face < 6; ++face)
            {
                if (!source->ReadbackPixels(image.Mips[0][face], selection.Mip, face))
                {
                    CORE_LOG_WARN("ReflectionProbe preview: readback failed for '{}' (mip {}, face {})", name, selection.Mip, face);
                    return nullptr;
                }
            }

            return ReflectionProbe::BuildPreviewTexture(name, image);
        }
    }

    void ReflectionProbe::RequestBakeCacheWrite(const std::string& path)
    {
        m_BakeCachePath = path;
        m_CacheWriteFailed = false;

        if (path.empty())
        {
            m_CacheWriteState = BakeCacheWriteState::Idle;
            return;
        }

        m_CacheWriteState = BakeCacheWriteState::Requested;
    }

    std::string ReflectionProbe::MakeDefaultBakeCachePath(const std::string& scene_path) const
    {
        std::string name = GetDebugName();
        if (name.empty())
            name = "ReflectionProbe";

        /* 只把真正非法的字符替换掉；UTF-8 多字节序列原样保留，中文名可用 */
        for (char& character : name)
        {
            const unsigned char code = static_cast<unsigned char>(character);
            if (code >= 0x80)
                continue;
            const bool safe = (code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z')
                || (code >= '0' && code <= '9') || code == '_' || code == '-';
            if (!safe)
                character = '_';
        }

        /* 缓存子目录 = 场景相对资源根的路径（去扩展名）：镜像场景在资源树里的位置，
         * 场景另存/搬移后缓存目录跟着走。场景不在资源根下时相对路径会带 .. ——
         * 不能把它拼进资源相对路径，退回只用场景文件名起一个目录。 */
        std::string scene_directory;
        std::filesystem::path scene_file;
        if (!scene_path.empty() && TryPathFromUtf8(scene_path, scene_file))
        {
            std::error_code error;
            const std::filesystem::path relative = std::filesystem::relative(scene_file, g_AssetsPath, error);
            const bool under_assets_root = !error && !relative.empty()
                && std::none_of(relative.begin(), relative.end(),
                    [](const std::filesystem::path& part) { return part == ".."; });

            scene_directory = PathToUtf8(under_assets_root
                ? (relative.parent_path() / relative.stem())
                : scene_file.stem());
        }

        const std::string directory = scene_directory.empty()
            ? "BakedReflectionProbes/"
            : "BakedReflectionProbes/" + scene_directory + "/";
        return directory + name + ".probe";
    }

    void ReflectionProbe::TickBakeCacheWrite()
    {
        if (m_CacheWriteState == BakeCacheWriteState::Idle)
            return;

        if (m_CacheWriteState == BakeCacheWriteState::Requested)
        {
            /* 等这一次烘焙真正完成（未烘焙的探针会由 Manager 在本帧或下一帧排入烘焙） */
            if (!m_BakeCompleted)
                return;

            m_CacheWriteState = BakeCacheWriteState::WaitingForGPU;
            m_CacheWriteCountdown = kBakeCacheWriteFrameDelay;
            return;
        }

        if (m_CacheWriteCountdown > 0)
        {
            --m_CacheWriteCountdown;
            return;
        }

        /* 承载烘焙绘制的命令缓冲区此时已提交且 GPU 可能仍在执行，
         * 直接回读会拿到旧内容，因此先等它执行完。 */
        if (auto render_api = Renderer::GetRenderAPI())
            render_api->WaitForGPU();

        m_CacheWriteFailed = !SaveBakeCache(ABSOLUTE_PATH(m_BakeCachePath));
        m_CacheWriteState = BakeCacheWriteState::Idle;
    }

    bool ReflectionProbe::SaveBakeCache(const std::string& path)
    {
        PROFILE_FUNCTION();

        if (path.empty())
            return false;

        ReflectionProbeBakeCache::Data data;

        const auto append_image = [&data](ReflectionProbeBakeCache::ImageKind kind, const SharedPtr<DeviceTexture>& texture)
        {
            if (texture == nullptr)
                return true;

            ReflectionProbeBakeCache::Image image;
            image.Kind = kind;
            image.Format = texture->GetTextureDesc().Format;
            image.Size = texture->GetWidth();

            const uint32_t mip_levels = std::max<uint32_t>(1u, texture->GetTextureDesc().MipLevels);
            for (uint32_t mip = 0; mip < mip_levels; ++mip)
            {
                std::vector<std::vector<uint8_t>> faces(ReflectionProbeBakeCache::kFaceCount);
                for (uint32_t face = 0; face < ReflectionProbeBakeCache::kFaceCount; ++face)
                {
                    if (!texture->ReadbackPixels(faces[face], mip, face))
                        return false;
                }

                image.Mips.emplace_back(std::move(faces));
            }

            data.Images.emplace_back(std::move(image));
            return true;
        };

        if (!append_image(ReflectionProbeBakeCache::ImageKind::Environment, m_BakeResult.EnvColorCubemap))
            return false;

        if (m_BakeConfig.BakeDiffuse
            && !append_image(ReflectionProbeBakeCache::ImageKind::Irradiance, m_BakeResult.IrradianceMap))
            return false;

        if (m_BakeConfig.BakeSpecular
            && !append_image(ReflectionProbeBakeCache::ImageKind::Prefilter, m_BakeResult.PrefilterMap))
            return false;

        if (!data.IsValid())
        {
            CORE_LOG_WARN("ReflectionProbe '{}': nothing to cache, bake it first", GetDebugName());
            return false;
        }

        const bool succeeded = ReflectionProbeBakeCache::Write(path, data);
        if (succeeded)
            CORE_LOG_INFO("ReflectionProbe '{}': bake result cached to '{}'", GetDebugName(), path);

        return succeeded;
    }

    bool ReflectionProbe::LoadBakeCache(const std::string& path)
    {
        PROFILE_FUNCTION();

        ReflectionProbeBakeCache::Data data;
        if (!ReflectionProbeBakeCache::Read(path, data))
            return false;

        Reset();

        const auto restore_image = [this](const ReflectionProbeBakeCache::Image& image) -> SharedPtr<DeviceTexture>
        {
            PixelDesc pixel_desc;
            if (!ResolveBakeCachePixelDesc(image.Format, pixel_desc))
            {
                CORE_LOG_ERROR("ReflectionProbe '{}': cache format is not uploadable", GetDebugName());
                return nullptr;
            }

            TextureDesc desc;
            desc.SamplerType = SamplerType::SamplerCubeMap;
            desc.Width = image.Size;
            desc.Height = image.Size;
            desc.Format = image.Format;
            desc.Usage = TextureUsage::ColorAttachment | TextureUsage::Sampleable;
            desc.MipLevels = static_cast<uint8_t>(image.Mips.size());

            SharedPtr<DeviceTexture> texture = DeviceTexture::Create(GetDebugName() + "_Cached", desc);
            if (texture == nullptr)
                return nullptr;

            for (uint32_t mip = 0; mip < image.Mips.size(); ++mip)
            {
                for (uint32_t face = 0; face < ReflectionProbeBakeCache::kFaceCount; ++face)
                {
                    const auto& face_data = image.Mips[mip][face];
                    texture->SetData(const_cast<uint8_t*>(face_data.data()), pixel_desc, mip, 0, 0, face);
                }
            }

            return texture;
        };

        for (const ReflectionProbeBakeCache::Image& image : data.Images)
        {
            const SharedPtr<DeviceTexture> texture = restore_image(image);
            if (texture == nullptr)
            {
                Reset();
                return false;
            }

            switch (image.Kind)
            {
            case ReflectionProbeBakeCache::ImageKind::Environment: m_BakeResult.EnvColorCubemap = texture; break;
            case ReflectionProbeBakeCache::ImageKind::Irradiance:  m_BakeResult.IrradianceMap = texture;   break;
            case ReflectionProbeBakeCache::ImageKind::Prefilter:   m_BakeResult.PrefilterMap = texture;    break;
            }
        }

        /* 结果已就绪，无需再烘焙 */
        m_BakeCompleted = true;
        CORE_LOG_INFO("ReflectionProbe '{}': bake result restored from '{}'", GetDebugName(), path);
        return true;
    }

    /* ==================== 烘焙结果预览 ==================== */

    void ReflectionProbe::RequestBakePreview()
    {
        if (m_PreviewState != BakePreviewState::Idle)
            return; /* 已在生成中：幂等 */

        m_PreviewFailed = false;
        m_PreviewState = BakePreviewState::Requested;
    }

    void ReflectionProbe::TickBakePreview()
    {
        /* 重烘焙请求：UI 帧里点的（当帧绘制列表可能还引用着预览纹理，
         * 当场销毁会悬垂），在这里统一执行 —— Prepare 先于本帧的绘制/UI */
        if (m_RebakeRequested)
        {
            m_RebakeRequested = false;
            Reset();
        }

        if (m_PreviewState == BakePreviewState::Idle)
            return;

        if (m_PreviewState == BakePreviewState::Requested)
        {
            /* 等烘焙结果就绪（请求可能先于烘焙完成到达，例如同一帧里先点 Rebake） */
            if (!m_BakeCompleted)
                return;

            m_PreviewState = BakePreviewState::WaitingForGPU;
            m_PreviewCountdown = kBakePreviewFrameDelay;
            return;
        }

        if (m_PreviewCountdown > 0)
        {
            --m_PreviewCountdown;
            return;
        }

        /* 承载烘焙绘制的命令缓冲区可能还在执行：回读前先等它执行完（与缓存写入同因） */
        if (auto render_api = Renderer::GetRenderAPI())
            render_api->WaitForGPU();

        m_PreviewFailed = !BuildBakePreviews();
        m_PreviewState = BakePreviewState::Idle;
    }

    bool ReflectionProbe::BuildBakePreviews()
    {
        constexpr size_t count = kBakePreviewCount;
        static constexpr const char* kNames[count] = { "_EnvPreview", "_IrradiancePreview", "_PrefilterPreview" };

        const SharedPtr<DeviceTexture>* sources[count] = {
            &m_BakeResult.EnvColorCubemap,
            &m_BakeResult.IrradianceMap,
            &m_BakeResult.PrefilterMap,
        };

        /* 全部构建成功才一次性换上：避免一半新一半旧 */
        SharedPtr<DeviceTexture> built[count];
        for (size_t index = 0; index < count; ++index)
        {
            if (*sources[index] == nullptr)
                continue; /* 该图未烘焙（配置省略）：预览也没有 */

            built[index] = BuildGpuBakePreviewTexture(GetDebugName() + kNames[index], *sources[index]);
            if (built[index] == nullptr)
                return false;
        }

        for (size_t index = 0; index < count; ++index)
            m_BakePreviews[index] = std::move(built[index]);

        return m_BakePreviews[0] != nullptr;
    }

    /* ==================== 烘焙结果预览（公共构建） ====================
     * 输入是一张立方图的原始字节（来自 GPU 回读、或直接读 .probe 缓存文件），
     * 输出是可直接给 UI 采样的十字展开 RGBA8 纹理。 */

    SharedPtr<DeviceTexture> ReflectionProbe::BuildPreviewTexture(const std::string& name,
        const ReflectionProbeBakeCache::Image& image, int mip_index)
    {
        /* 没有渲染后端实例（headless 检查程序没调过 Renderer::Init）：无处上传 2D 纹理。
         * Metal 的纹理/回读全依赖 Init 时注册的设备，这里先短路，别往下走。 */
        if (Renderer::GetRenderAPI() == nullptr)
            return nullptr;

        const uint32_t bytes_per_texel = GetTextureFormatTexelSize(image.Format);
        if (bytes_per_texel == 0)
            return nullptr;

        /* 选 mip：显式指定就用指定层（资源详情的手动切换）——越界视为数据不完整；
         * 否则自动取封面到 ≤64px 的那层 */
        const uint32_t mip_count = static_cast<uint32_t>(std::max<size_t>(image.Mips.size(), 1));
        const uint32_t mip = (mip_index < 0)
            ? SelectPreviewMip(image.Size, mip_count).Mip
            : static_cast<uint32_t>(mip_index);

        if (mip >= image.Mips.size() || image.Mips[mip].size() < 6)
            return nullptr;

        const uint32_t face_size = PreviewMipFaceSize(image.Size, mip);
        const size_t expected_face_bytes = static_cast<size_t>(face_size) * face_size * bytes_per_texel;
        for (const std::vector<uint8_t>& face : image.Mips[mip])
        {
            if (face.size() < expected_face_bytes)
                return nullptr;
        }

        const uint32_t cross_width = face_size * 4;
        const uint32_t cross_height = face_size * 3;
        /* 空位保持全 0（alpha 0）：显示时透出面板底色，十字形状一眼可辨 */
        std::vector<uint8_t> pixels(static_cast<size_t>(cross_width) * cross_height * 4, 0);

        for (uint32_t face = 0; face < 6; ++face)
        {
            const std::vector<uint8_t>& raw = image.Mips[mip][face];
            const uint32_t base_x = kCrossColumn[face] * face_size;
            const uint32_t base_y = kCrossRow[face] * face_size;

            for (uint32_t y = 0; y < face_size; ++y)
            {
                for (uint32_t x = 0; x < face_size; ++x)
                {
                    const uint8_t* texel = raw.data() + (static_cast<size_t>(y) * face_size + x) * bytes_per_texel;

                    float r = 0.0f;
                    float g = 0.0f;
                    float b = 0.0f;
                    switch (image.Format)
                    {
                    case TextureFormat::RGBA16F:
                    {
                        uint16_t half[4] = {};
                        std::memcpy(half, texel, sizeof(half));
                        r = HalfToFloat(half[0]);
                        g = HalfToFloat(half[1]);
                        b = HalfToFloat(half[2]);
                        break;
                    }
                    case TextureFormat::RGBA32F:
                    {
                        float rgba[4] = {};
                        std::memcpy(rgba, texel, sizeof(rgba));
                        r = rgba[0];
                        g = rgba[1];
                        b = rgba[2];
                        break;
                    }
                    default:
                        return nullptr;
                    }

                    uint8_t* dst = pixels.data()
                        + (static_cast<size_t>(base_y + y) * cross_width + (base_x + x)) * 4;
                    dst[0] = TonemapToByte(r);
                    dst[1] = TonemapToByte(g);
                    dst[2] = TonemapToByte(b);
                    dst[3] = 255;
                }
            }
        }

        TextureDesc preview_desc;
        preview_desc.SamplerType = SamplerType::Sampler2D;
        preview_desc.Width = cross_width;
        preview_desc.Height = cross_height;
        preview_desc.Format = TextureFormat::RGBA8;
        preview_desc.Usage = TextureUsage::Sampleable;
        preview_desc.MipLevels = 1;

        SharedPtr<DeviceTexture> preview = DeviceTexture::Create(name, preview_desc);
        if (preview == nullptr)
            return nullptr;

        preview->SetData(pixels.data(), PixelDesc{ PixelFormat::RGBA, PixelType::UnsignedByte });
        return preview;
    }

    uint32_t ReflectionProbe::AutoPreviewMip(const ReflectionProbeBakeCache::Image& image)
    {
        return SelectPreviewMip(image.Size,
            static_cast<uint32_t>(std::max<size_t>(image.Mips.size(), 1))).Mip;
    }

    uint32_t ReflectionProbe::PreviewMipFaceSize(uint32_t mip0_size, uint32_t mip_index)
    {
        return std::max<uint32_t>(mip0_size >> std::min(mip_index, 31u), 1u);
    }

    /* 构造一个带 Color0(立方体贴图) + Depth 的离屏帧缓冲，用于把场景渲染到立方体贴图的某一面。 */
    SharedPtr<DeviceFrameBuffer> ReflectionProbe::MakeSceneCaptureFrameBuffer(const SharedPtr<DeviceTexture>& color_target, const SharedPtr<DeviceTexture>& depth_target, uint32_t size)
    {
        PROFILE_FUNCTION();

        RenderBufferInfo color_info;
        color_info.RenderTarget = color_target;
        color_info.Level = 0;
        color_info.Layer = 0;

        RenderBufferInfo depth_info;
        depth_info.RenderTarget = depth_target;
        depth_info.Level = 0;
        depth_info.Layer = 0;

        FrameBufferDesc desc;
        desc.Samples = 1;
        desc.Usage = RenderBufferUsage::ColorDefault;
        if (depth_target) desc.Usage |= RenderBufferUsage::Depth;
        desc.ViewportRegion = ViewportRegion{ 0, 0, size, size };
        desc.ColorRenderBuffers.push_back(color_info);
        desc.DepthRenderBuffer = depth_info;

        return DeviceFrameBuffer::Create(GetDebugName() + "_CaptureFrameBuffer", desc);
    }

    /* 立方体捕获相机朝向（+X, -X, +Y, -Y, +Z, -Z） */
    const static glm::mat4 GetCaptureViewMatrix(uint32_t face_index)
    {
        const glm::vec3 eye = glm::vec3(0.0f);
        switch (face_index)
        {
        case 0: return glm::lookAt(eye, glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)); // +X
        case 1: return glm::lookAt(eye, glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)); // -X
        case 2: return glm::lookAt(eye, glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)); // +Y
        case 3: return glm::lookAt(eye, glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)); // -Y
        case 4: return glm::lookAt(eye, glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, -1.0f, 0.0f)); // +Z
        case 5: return glm::lookAt(eye, glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f)); // -Z
        default: return glm::mat4(1.0f);
        }
    }

    void ReflectionProbe::BakeEnvCubemap(RenderView* render_view)
    {
        PROFILE_FUNCTION();

        const std::string label = GetDebugName() + "_BakeEnvCubemap";
        Renderer::GetRenderAPI()->PushDebugGroup(label.c_str());

        const uint32_t size = m_BakeConfig.EnvSize;
        if (!m_BakeResult.EnvColorCubemap || m_BakeResult.EnvColorCubemap->GetWidth() != size || m_BakeResult.EnvColorCubemap->GetHeight() != size)
        {
            TextureDesc env_desc;
            env_desc.SamplerType = SamplerType::SamplerCubeMap;
            env_desc.Width = size;
            env_desc.Height = size;
            env_desc.Format = TextureFormat::RGBA16F;
            env_desc.Usage = TextureUsage::ColorAttachment | TextureUsage::Sampleable;
            /* 完整 mip 链：预滤波按采样立体角选 LOD 时需要更模糊的源层级，
             * 只渲染 mip0 会让极亮小特征（太阳）走样成孤立白斑 */
            env_desc.MipLevels = static_cast<uint8_t>(std::floor(std::log2(static_cast<float>(size)))) + 1;
            m_BakeResult.EnvColorCubemap = DeviceTexture::Create(GetDebugName() + "_EnvColorCubemap", env_desc);
        }

        /* 仅烘焙天空盒 */
        if (m_BakeConfig.BakeSkyBoxOnly && m_pSkyBoxTexture)
        {
            auto shader = DeviceShader::Create(ABSOLUTE_PATH("Shaders/ReflectionProbe/EquirectToCube.glsl"));
            auto material = Material::Create(shader);
            material->SetTexture("u_EquirectangularMap", m_pSkyBoxTexture);

            auto capture_fb = MakeSceneCaptureFrameBuffer(m_BakeResult.EnvColorCubemap, nullptr, size);
            for (int face = 0; face < 6; ++face)
            {
                capture_fb->Bind(FrameBufferBindInfo::ToColorLayer(0, face, 0));
                Renderer::Clear();

                material->SetParameters(ParamType::Int, "u_FaceId", face);
                Renderer::Submit(material, Renderer::GetFullScreenVertexArray());
            }
            capture_fb->Unbind();

            Renderer::GetRenderAPI()->PopDebugGroup();
            return;
        }

        /* 烘焙场景：在探针位置用6个朝向相机实时捕获场景 */
        if (!m_BakeResult.EnvDepthCubemap || m_BakeResult.EnvDepthCubemap->GetWidth() != size || m_BakeResult.EnvDepthCubemap->GetHeight() != size)
        {
            TextureDesc depth_desc;
            depth_desc.SamplerType = SamplerType::Sampler2D;
            depth_desc.Width = size;
            depth_desc.Height = size;
            depth_desc.Format = TextureFormat::Depth24;
            depth_desc.Usage = TextureUsage::DepthAttachment;
            depth_desc.MipLevels = 1;
            m_BakeResult.EnvDepthCubemap = DeviceTexture::Create(GetDebugName() + "_EnvDepthCubemap", depth_desc);
        }

        const float aspect = 1.0f;
        const float near_plane = 0.1f;
        const float far_plane = 1000.0f;
        const glm::mat4 capture_projection = MakeReversedZProjection(glm::perspective(glm::radians(90.0f), aspect, near_plane, far_plane));
        const glm::vec3 capture_position = GetPosition();

        const auto& visible_objects = render_view->GetVisibleMeshObjects();

        auto capture_fb = MakeSceneCaptureFrameBuffer(m_BakeResult.EnvColorCubemap, m_BakeResult.EnvDepthCubemap, size);
        for (uint32_t face = 0; face < 6; ++face)
        {
            capture_fb->Bind(FrameBufferBindInfo::ToColorLayer(0, face, 0));
            Renderer::Clear();

            const glm::mat4 view = GetCaptureViewMatrix(face);
            /* 捕获相机在探针位置：视图 = 朝向 × 平移(-探针位置)（标准 lookAt 约定
             * R·T(-eye)）——只给朝向矩阵会让捕获发生在世界原点，探针离原点越远、
             * 烘焙出的环境越离谱。 */
            Renderer::SetViewUniforms(
                view * glm::translate(glm::mat4(1.0f), -capture_position),
                capture_projection, capture_position);

            for (const auto& mesh_object : visible_objects)
            {
                Renderer::FillObjectUniformBuffer(mesh_object);
                const auto& material = mesh_object.Material;
                if (material == nullptr || material->GetShader() == nullptr)
                    continue;

                /* 捕获阶段关闭 IBL：走 per-draw 覆盖、不写共享材质；只对支持 IBL 的 Shader 发，覆盖里同时
                 * 带中性兜底 —— 只把 u_UseIBL 置零会留下没绑定的采样器，而绘制时声明过的采样器必须全部有绑定。 */
                DrawParams per_draw;
                if (material->SupportsIBL())
                    Material::MakeIBLParamOverrides(per_draw.Overrides, nullptr, nullptr, nullptr);
                Renderer::Submit(material, mesh_object.MeshSegment->GetMeshPrimitive(),
                    per_draw.Overrides.empty() ? nullptr : &per_draw);
            }
        }
        capture_fb->Unbind();

        /* 捕获过程中改写了全局共享的 View 常量（上面每面都调用 SetViewUniforms 换成
         * 捕获相机）。ViewUniformBuffer 是全局唯一的，若不恢复，本 Pass 之后的
         * ScenePass 会继续用最后一个捕获面的视角（探针位置、90° FOV）绘制主视图。 */
        if (render_view)
        {
            if (const Camera* culling_camera = render_view->GetCullingCamera())
            {
                Renderer::SetViewUniforms(culling_camera->GetViewMatrix(),
                    culling_camera->GetProjectionMatrix(), culling_camera->GetPosition());
            }
        }

        Renderer::GetRenderAPI()->PopDebugGroup();
    }

    void ReflectionProbe::BakeIrradianceMap()
    {
        PROFILE_FUNCTION();

        const std::string label = GetDebugName() + "_BakeIrradianceMap";
        Renderer::GetRenderAPI()->PushDebugGroup(label.c_str());

        const uint32_t size = m_BakeConfig.IrradianceSize;
        if (!m_BakeResult.IrradianceMap || m_BakeResult.IrradianceMap->GetWidth() != size || m_BakeResult.IrradianceMap->GetHeight() != size)
        {
            TextureDesc irradiance_desc;
            irradiance_desc.SamplerType = SamplerType::SamplerCubeMap;
            irradiance_desc.Width = size;
            irradiance_desc.Height = size;
            irradiance_desc.Format = TextureFormat::RGBA16F;
            irradiance_desc.Usage = TextureUsage::ColorAttachment | TextureUsage::Sampleable;
            irradiance_desc.MipLevels = 1;
            m_BakeResult.IrradianceMap = DeviceTexture::Create(GetDebugName() + "_IrradianceMap", irradiance_desc);
        }

        auto shader = DeviceShader::Create(ABSOLUTE_PATH("Shaders/ReflectionProbe/Irradiance.glsl"));
        auto material = Material::Create(shader);
        material->SetTexture("u_EnvironmentMap", m_BakeResult.EnvColorCubemap);

        /* 源环境立方图的最大 mip（= log2(size)）：卷积据此换算出源分辨率，
         * 再按每次采样的立体角选源 mip，避免点采样命中太阳这类极亮 texel。 */
        const uint32_t env_mip_levels = m_BakeResult.EnvColorCubemap
            ? m_BakeResult.EnvColorCubemap->GetTextureDesc().MipLevels
            : 1u;
        material->SetParameters(ParamType::Float, "u_EnvironmentMapMaxMip",
            static_cast<float>(env_mip_levels > 0 ? env_mip_levels - 1u : 0u));

        auto fb = MakeSceneCaptureFrameBuffer(m_BakeResult.IrradianceMap, nullptr, size);
        for (int face = 0; face < 6; ++face)
        {
            fb->Bind(FrameBufferBindInfo::ToColorLayer(0, face, 0));
            Renderer::Clear();
            material->SetParameters(ParamType::Int, "u_FaceId", face);
            Renderer::Submit(material, Renderer::GetFullScreenVertexArray());
        }
        fb->Unbind();

        Renderer::GetRenderAPI()->PopDebugGroup();
    }

    void ReflectionProbe::BakePrefilterMap()
    {
        PROFILE_FUNCTION();

        const std::string label = GetDebugName() + "_BakePrefilterMap";
        Renderer::GetRenderAPI()->PushDebugGroup(label.c_str());

        const uint32_t size = m_BakeConfig.PrefilterSize;
        const uint32_t mip_levels = m_BakeConfig.PrefilterMipLevels > 0
            ? m_BakeConfig.PrefilterMipLevels
            : static_cast<uint32_t>(std::floor(std::log2(static_cast<float>(m_BakeConfig.PrefilterSize)))) + 1; /* 计算实际层级数 */

        if (!m_BakeResult.PrefilterMap || m_BakeResult.PrefilterMap->GetWidth() != size || m_BakeResult.PrefilterMap->GetHeight() != size
            || m_BakeResult.PrefilterMap->GetTextureDesc().MipLevels != mip_levels)
        {
            TextureDesc prefilter_desc;
            prefilter_desc.SamplerType = SamplerType::SamplerCubeMap;
            prefilter_desc.Width = size;
            prefilter_desc.Height = size;
            prefilter_desc.Format = TextureFormat::RGBA16F;
            prefilter_desc.Usage = TextureUsage::ColorAttachment | TextureUsage::Sampleable;
            prefilter_desc.MipLevels = mip_levels;
            m_BakeResult.PrefilterMap = DeviceTexture::Create(GetDebugName() + "_PrefilterMap", prefilter_desc);
        }

        auto shader = DeviceShader::Create(ABSOLUTE_PATH("Shaders/ReflectionProbe/Prefilter.glsl"));
        auto material = Material::Create(shader);
        material->SetTexture("u_EnvironmentMap", m_BakeResult.EnvColorCubemap);

        /* 源环境立方图的最大 mip：预滤波按 u_Roughness * MaxMip 选取采样层级。 */
        const uint32_t env_mip_levels = m_BakeResult.EnvColorCubemap
            ? m_BakeResult.EnvColorCubemap->GetTextureDesc().MipLevels
            : 1u;
        material->SetParameters(ParamType::Float, "u_EnvironmentMapMaxMip",
            static_cast<float>(env_mip_levels > 0 ? env_mip_levels - 1u : 0u));

        auto fb = MakeSceneCaptureFrameBuffer(m_BakeResult.PrefilterMap, nullptr, size);
        for (uint32_t mip = 0; mip < mip_levels; ++mip)
        {
            const uint32_t mip_size = static_cast<uint32_t>(size * std::pow(0.5f, static_cast<float>(mip)));
            const float roughness = static_cast<float>(mip) / static_cast<float>(mip_levels - 1);
            material->SetParameters(ParamType::Float, "u_Roughness", roughness);

            for (int face = 0; face < 6; ++face)
            {
                fb->Bind(FrameBufferBindInfo::ToColorLayer(0, face, mip));
                Renderer::SetViewport(0, 0, mip_size, mip_size);
                Renderer::Clear();

                material->SetParameters(ParamType::Int, "u_FaceId", face);
                Renderer::Submit(material, Renderer::GetFullScreenVertexArray());
            }
        }
        fb->Unbind();

        Renderer::GetRenderAPI()->PopDebugGroup();
    }

    void ReflectionProbeManager::RegisterProbe(const SharedPtr<ReflectionProbe>& probe)
    {
        PROFILE_FUNCTION();

        if (!probe)
            return;
        if (std::find(m_RegisteredProbes.begin(), m_RegisteredProbes.end(), probe) == m_RegisteredProbes.end())
            m_RegisteredProbes.push_back(probe);
    }

    void ReflectionProbeManager::UnregisterProbe(const SharedPtr<ReflectionProbe>& probe)
    {
        PROFILE_FUNCTION();

        auto it = std::find(m_RegisteredProbes.begin(), m_RegisteredProbes.end(), probe);
        if (it != m_RegisteredProbes.end())
            m_RegisteredProbes.erase(it);
    }

    void ReflectionProbeManager::ClearProbes()
    {
        PROFILE_FUNCTION();

        m_RegisteredProbes.clear();
        m_NeedBakeProbes.clear();
    }

    namespace
    {
        /* 探针当前可否用于 IBL 着色：启用、已烘焙、双图（辐照度 + 预滤波）齐备。
         * 缺图（烘焙配置关掉了漫反射 / 镜面）时按"不可用"处理 ——
         * 采样未绑定的立方图在 Metal 校验层直接断言，不做猜测性兜底。 */
        bool IsProbeUsableForIBL(const SharedPtr<ReflectionProbe>& probe)
        {
            return probe != nullptr && probe->GetEnable() && probe->IsBaked()
                && probe->GetIrradianceMap() != nullptr && probe->GetPrefilterMap() != nullptr;
        }
    }

    /* 获取距离最近的反射探针 */
    SharedPtr<ReflectionProbe> ReflectionProbeManager::GetClostedReflectionProbe(const glm::vec3& target_pos) const
    {
        PROFILE_FUNCTION();

        float best_dist_sq = std::numeric_limits<float>::max();
        SharedPtr<ReflectionProbe> chosen = nullptr;

        for (const auto& probe : m_RegisteredProbes)
        {
            if (!IsProbeUsableForIBL(probe))
                continue;
            const float dist_sq = glm::distance2(target_pos, probe->GetPosition());
            if (dist_sq < best_dist_sq)
            {
                best_dist_sq = dist_sq;
                chosen = probe;
            }
        }

        return chosen;
    }

    /* 按到目标点的距离升序收集可用的已烘焙探针（最多 max_count 个）。
     * 延迟光照在着色器里逐像素做"最近探针"选择，这里给出本帧参与选择的子集
     * （调用方传入相机位置：离视点更近的探针优先获得采样器槽位）。 */
    std::vector<SharedPtr<ReflectionProbe>> ReflectionProbeManager::CollectClosestBakedProbes(
        const glm::vec3& target_pos, size_t max_count) const
    {
        PROFILE_FUNCTION();

        std::vector<SharedPtr<ReflectionProbe>> usable;
        usable.reserve(m_RegisteredProbes.size());
        for (const auto& probe : m_RegisteredProbes)
        {
            if (IsProbeUsableForIBL(probe))
                usable.push_back(probe);
        }

        std::sort(usable.begin(), usable.end(),
            [&target_pos](const SharedPtr<ReflectionProbe>& lhs, const SharedPtr<ReflectionProbe>& rhs)
            {
                return glm::distance2(target_pos, lhs->GetPosition())
                    < glm::distance2(target_pos, rhs->GetPosition());
            });

        if (usable.size() > max_count)
            usable.resize(max_count);
        return usable;
    }

    void ReflectionProbeManager::Prepare()
    {
        PROFILE_FUNCTION();

        m_NeedBakeProbes.clear();

        /* 从已注册的反射探针中，筛选需要烘焙的（realtime 探针每帧烘焙，未烘焙的 dirty 探针需要烘焙） */
        for (const auto& probe : m_RegisteredProbes)
        {
            if (!probe || !probe->GetEnable())
                continue;

            /* 推进「烘焙结果落盘」状态机：等烘焙完成、再等 GPU 执行完，最后回读写出 */
            probe->TickBakeCacheWrite();

            /* 推进「烘焙结果预览」状态机：同样等烘焙完成 + GPU 执行完，再转换上传 */
            probe->TickBakePreview();

            if (probe->IsRealtime() || !probe->IsBaked())
                m_NeedBakeProbes.push_back(probe);
        }
    }

    void ReflectionProbeManager::RequestBakeCacheWrites(const std::string& scene_path)
    {
        PROFILE_FUNCTION();

        /* 同一场景允许存在重名探针（复制实体不会自动改名）：按登记顺序给重名者的
         * 缓存文件加 _2 / _3 … 序号，否则它们会互相覆盖彼此的烘焙结果。
         * 登记顺序与实体创建顺序一致，加载后仍然稳定。 */
        std::unordered_set<std::string> assigned_paths;

        for (const auto& probe : m_RegisteredProbes)
        {
            /* 没有烘焙结果就没有需要持久化的内容 */
            if (!probe || !probe->GetEnable() || !probe->IsBaked())
                continue;

            /* 写入目标在保存时重新推导（缓存目录跟随场景）：场景另存/搬移后缓存跟着走，
             * 场景文件里记录的旧路径只用于加载。 */
            std::string path = probe->MakeDefaultBakeCachePath(scene_path);

            if (!assigned_paths.insert(path).second)
            {
                constexpr char kExtension[] = ".probe";
                const std::string base = path.substr(0, path.size() - (sizeof(kExtension) - 1));
                for (uint32_t index = 2; ; ++index)
                {
                    std::string candidate = base + "_" + std::to_string(index) + kExtension;
                    if (assigned_paths.insert(candidate).second)
                    {
                        path = std::move(candidate);
                        break;
                    }
                }
            }

            probe->RequestBakeCacheWrite(path);
        }
    }

    void ReflectionProbeManager::AddBakeReflectionProbePass(RenderView* render_view)
    {
        PROFILE_FUNCTION();

        if (!render_view)
            return;

        auto& frame_graph = render_view->GetFrameGraph();
        if (!frame_graph)
            return;

        /* BRDFLut 是 IBL 的通用查找表：只要有已注册的探针就要保证它在 ——
         * 探针可能全部来自磁盘缓存（这一帧不需要任何烘焙 Pass），但 IBL 着色仍要
         * 用它。只需烘焙一次。 */
        if (m_BRDFLutMap == nullptr && !m_RegisteredProbes.empty())
            BakeBRDFLutMap();

        if (m_NeedBakeProbes.empty())
            return;

        /* Probe 捕获使用中性阴影图，不复用主相机的级联阴影。 */
        const auto no_shadow_map_handle = frame_graph->GetBlackboard()
            .GetResourceHandle<FrameGraphTexture>("NoShadowMapHandle");

        struct BakeProbePassData
        {
            FrameGraphResourceHandleTyped<FrameGraphTexture> NoShadowMap;
        };
        for (const auto& probe : m_NeedBakeProbes)
        {
            if (!probe)
                continue;

            frame_graph->AddPass<BakeProbePassData>("BakeReflectionProbePass",
                [no_shadow_map_handle](FrameGraphBuilder& builder, BakeProbePassData& data)
                {
                    data.NoShadowMap = no_shadow_map_handle;
                    builder.BindInputResource(data.NoShadowMap, FrameGraphTexture::Usage::Sampleable);
                    builder.AsSideEffect(true);
                },
                [probe, render_view](const FrameGraphResources& resources, const BakeProbePassData& data)
                {
                    ScopedShadowMapBinding shadow_map(resources.Get(data.NoShadowMap).Texture);
                    probe->Bake(render_view);
                });
        }
    }

    /* 烘焙BRDF查找表 */
    void ReflectionProbeManager::BakeBRDFLutMap()
    {
        PROFILE_FUNCTION();

        constexpr uint32_t BRDF_LUT_SIZE = 512;

        TextureDesc brdf_lut_desc;
        brdf_lut_desc.SamplerType = SamplerType::Sampler2D;
        brdf_lut_desc.Width = BRDF_LUT_SIZE;
        brdf_lut_desc.Height = BRDF_LUT_SIZE;
        brdf_lut_desc.Format = TextureFormat::RG16F;
        brdf_lut_desc.Usage = TextureUsage::ColorAttachment | TextureUsage::Sampleable;
        brdf_lut_desc.MipLevels = 1;
        m_BRDFLutMap = DeviceTexture::Create("BRDFLutMap", brdf_lut_desc);

        RenderBufferInfo color_info;
        color_info.RenderTarget = m_BRDFLutMap;
        color_info.Level = 0;
        color_info.Layer = 0;

        FrameBufferDesc desc;
        desc.Samples = 1;
        desc.Usage = RenderBufferUsage::ColorDefault;
        desc.ViewportRegion = ViewportRegion{ 0, 0, BRDF_LUT_SIZE, BRDF_LUT_SIZE };
        desc.ColorRenderBuffers.push_back(color_info);

        auto fb = DeviceFrameBuffer::Create("BRDFLutMap_FrameBuffer", desc);
        fb->Bind();
        {
            auto shader = DeviceShader::Create(ABSOLUTE_PATH("Shaders/ReflectionProbe/BRDFLut.glsl"));
            auto material = Material::Create(shader);

            Renderer::Clear();
            Renderer::Submit(material, MeshPrimitive(Renderer::GetFullScreenVertexArray()));
        }
        fb->Unbind();

    }
}
