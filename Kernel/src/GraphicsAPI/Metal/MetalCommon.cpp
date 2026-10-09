#ifdef PLATFORM_MACOS

#include "Pch.h"
#include "MetalCommon.h"
#include "Helios/Scene/SceneCommon.h"

namespace Helios
{
    namespace MetalRuntime
    {
        namespace
        {
            /* MetalRenderAPI 在构造/析构时注册与注销，生命周期覆盖所有 Device 对象 */
            MTL::Device* g_Device = nullptr;
            MTL::CommandQueue* g_CommandQueue = nullptr;

            /* 当前活动编码器由 RenderAPI 在 Begin/End RenderPass 时维护，
             * 供纹理更新、查询等操作判断是否可在渲染中途插入命令。 */
            MTL::RenderCommandEncoder* g_Encoder = nullptr;

            /* 当前帧序号，用于选取按帧切片的资源 */
            uint32_t g_FrameIndex = 0;

            /* ---- 管线编译产物缓存（MTLBinaryArchive）----
             * 懒加载单例：第一次请求时从磁盘加载，缺失 / 损坏就新建空缓存（Unavailable 标记避免反复重试）。
             * 只把真正新编译的条目写回；落盘在后续的窗口期触发。 */
            MTL::BinaryArchive* g_PipelineArchive = nullptr;
            bool g_PipelineArchiveUnavailable = false;
            bool g_PipelineArchiveDirty = false;
            bool g_PipelineArchiveWriteWarned = false;
            double g_PipelineArchiveLastWrite = 0.0;
            double g_PipelineArchiveNewestAdd = 0.0;

            /* 写盘节流窗口：既控制整档重写频率，也作为新条目的「静置期」——
             * 新条目的编译产物登记进驱动需要一点时间，静置后才允许卷入序列化，
             * 避免 GPUArchiver 打包偶发失败（如 “expecting 'fragment' stage”）。 */
            constexpr double kPipelineArchiveWriteInterval = 30.0;

            double NowSeconds()
            {
                return std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            }

            std::filesystem::path PipelineArchivePath()
            {
                return g_AssetsPath / "Cache/Shaders/PipelineArchive.binary";
            }

            /* 把未落盘的缓存条目写出；失败保留脏标记（下次窗口再试），
             * 告警一次避免重试期间刷屏 */
            void WritePipelineArchive()
            {
                if (g_PipelineArchive == nullptr || !g_PipelineArchiveDirty)
                    return;

                const std::filesystem::path path = PipelineArchivePath();
                std::error_code error_code;
                if (path.has_parent_path())
                    std::filesystem::create_directories(path.parent_path(), error_code);

                NS::Error* error = nullptr;
                NS::URL* url = NS::URL::fileURLWithPath(
                    NS::String::string(PathToUtf8(path).c_str(), NS::UTF8StringEncoding));
                if (g_PipelineArchive->serializeToURL(url, &error))
                {
                    g_PipelineArchiveDirty = false;
                    g_PipelineArchiveLastWrite = NowSeconds();
                    g_PipelineArchiveWriteWarned = false;
                }
                else if (!g_PipelineArchiveWriteWarned)
                {
                    g_PipelineArchiveWriteWarned = true;
                    CORE_LOG_WARN("MetalRuntime: failed to serialize pipeline archive: {}",
                        error && error->localizedDescription()
                            ? error->localizedDescription()->utf8String() : "unknown");
                }
            }
        }

        void Register(MTL::Device* device, MTL::CommandQueue* queue)
        {
            g_Device = device;
            g_CommandQueue = queue;
            g_Encoder = nullptr;
            g_FrameIndex = 0;
        }

        void Unregister()
        {
            /* 先把缓存的新条目落盘并释放，再清运行时上下文（此后设备不可再用） */
            FlushPipelineArchive();
            if (g_PipelineArchive != nullptr)
            {
                g_PipelineArchive->release();
                g_PipelineArchive = nullptr;
            }
            g_PipelineArchiveUnavailable = false;
            g_PipelineArchiveDirty = false;
            g_PipelineArchiveWriteWarned = false;
            g_PipelineArchiveLastWrite = 0.0;
            g_PipelineArchiveNewestAdd = 0.0;

            g_Device = nullptr;
            g_CommandQueue = nullptr;
            g_Encoder = nullptr;
            g_FrameIndex = 0;
        }

        MTL::Device* Device()
        {
            return g_Device;
        }

        MTL::CommandQueue* Queue()
        {
            return g_CommandQueue;
        }

        MTL::RenderCommandEncoder* Encoder()
        {
            return g_Encoder;
        }

        void SetEncoder(MTL::RenderCommandEncoder* encoder)
        {
            g_Encoder = encoder;
        }

        bool IsAvailable()
        {
            return g_Device != nullptr;
        }

        uint32_t FrameIndex()
        {
            return g_FrameIndex;
        }

        void AdvanceFrame()
        {
            g_FrameIndex = (g_FrameIndex + 1) % MetalConfig::MaxFramesInFlight;
        }

        MTL::BinaryArchive* PipelineArchive()
        {
            if (g_PipelineArchive != nullptr || g_PipelineArchiveUnavailable || g_Device == nullptr)
                return g_PipelineArchive;

            MTL::BinaryArchiveDescriptor* descriptor = MTL::BinaryArchiveDescriptor::alloc()->init();
            NS::Error* error = nullptr;

            /* 先尝试加载磁盘缓存；文件缺失（首次运行）/ 损坏 / 不兼容时退回空缓存重新积累 */
            NS::URL* url = NS::URL::fileURLWithPath(
                NS::String::string(PathToUtf8(PipelineArchivePath()).c_str(), NS::UTF8StringEncoding));
            descriptor->setUrl(url);
            g_PipelineArchive = g_Device->newBinaryArchive(descriptor, &error);

            if (g_PipelineArchive == nullptr)
            {
                descriptor->setUrl(static_cast<NS::URL*>(nullptr));
                g_PipelineArchive = g_Device->newBinaryArchive(descriptor, &error);
            }
            descriptor->release();

            if (g_PipelineArchive == nullptr)
            {
                g_PipelineArchiveUnavailable = true;
                CORE_LOG_WARN("MetalRuntime: pipeline archive unavailable, shader compilation cache disabled");
            }

            /* 写盘节流从装载时刻起算：启动 / 开场景的编译高峰期内不做整档重写 */
            g_PipelineArchiveLastWrite = NowSeconds();

            return g_PipelineArchive;
        }

        void NotifyPipelineCompiled(MTL::RenderPipelineDescriptor* descriptor)
        {
            if (g_PipelineArchive == nullptr || descriptor == nullptr)
                return;

            /* 先把此前积累、且已静置过的新条目落盘（不含本条）：与添加同步触发的
             * 序列化可能因新条目产物尚未登记而整档失败；只让静置 ≥30s 的条目
             * 卷入本轮序列化，本条留给之后的窗口或退出时统一落盘。 */
            const double now = NowSeconds();
            if (g_PipelineArchiveDirty
                && now - g_PipelineArchiveNewestAdd >= kPipelineArchiveWriteInterval
                && now - g_PipelineArchiveLastWrite >= kPipelineArchiveWriteInterval)
                WritePipelineArchive();

            NS::Error* error = nullptr;
            if (!g_PipelineArchive->addRenderPipelineFunctions(descriptor, &error))
            {
                /* 拒绝添加只影响本条目入缓存，不影响运行；只告警一次 */
                static bool warned = false;
                if (!warned)
                {
                    warned = true;
                    CORE_LOG_WARN("MetalRuntime: pipeline archive add rejected: {}",
                        error && error->localizedDescription()
                            ? error->localizedDescription()->utf8String() : "unknown");
                }
                return;
            }

            g_PipelineArchiveDirty = true;
            g_PipelineArchiveNewestAdd = now;
        }

        void FlushPipelineArchive()
        {
            if (g_PipelineArchiveDirty)
                WritePipelineArchive();
        }
    }
}

#endif /* PLATFORM_MACOS */
