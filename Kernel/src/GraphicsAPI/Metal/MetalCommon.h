#pragma once

#ifdef PLATFORM_MACOS

#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include "Helios/Core/Logger.h"
#include "Helios/Renderer/RenderCommon.h"
#include "Helios/VirtualDevice/DeviceBuffer.h"
#include "MetalBindings.h"
#include "MetalConversions.h"

namespace Helios
{
    /* 绑定槽位约定见 MetalBindings.h：Metal 的 buffer 绑定是单一命名空间（顶点和 uniform 共用），
     * 不同语义必须错开。纹理和采样器各有独立命名空间，而且 combined sampler 的 binding 同时用于
     * [[texture(N)]] 和 [[sampler(N)]]，直接沿用 GLSL binding 就行、不用偏移。 */

    /* ============================ 资源存储模式 ============================
     * 缓冲区用 Managed（写入后 didModifyRange 显式同步，集成 / 离散 GPU 都稳）。纹理用 Shared
     * （replaceRegion 后不用 blit 同步，免得 Managed 纹理在 macOS 上漏同步、读到旧数据）。 */
    namespace MetalStorage
    {
        static constexpr MTL::ResourceOptions BufferOptions = MTL::ResourceStorageModeManaged;
        static constexpr MTL::ResourceOptions TextureOptions = MTL::ResourceStorageModeShared;

        /* uniform 数据每帧由 CPU 覆写，Shared 模式省去 didModifyRange 的显式同步，
         * 且在统一内存（Apple Silicon）与离散 GPU 上都能正确工作。 */
        static constexpr MTL::ResourceOptions UniformBufferOptions = MTL::ResourceStorageModeShared;

        /* 非 4KB 对齐的 uniform 环缓冲区按帧切片时使用的对齐粒度 */
        static constexpr uint32_t UniformSliceAlignment = 256;
    }

    /* ============================ 全局渲染配置 ============================ */
    namespace MetalConfig
    {
        /* 同时在途的最大帧数。CPU 录制第 N 帧时 GPU 可能仍在读取第 N-1 帧，
         * uniform 数据因此按帧切片，避免跨帧覆写造成的数据竞争。 */
        static constexpr uint32_t MaxFramesInFlight = 3;
    }

    /* ============================ 运行时上下文 ============================
     * 由 MetalRenderAPI 在初始化/销毁时注册与注销，使各 Device 子类无需
     * dynamic_cast 即可安全取得设备、命令队列与当前编码器。 */
    namespace MetalRuntime
    {
        void Register(MTL::Device* device, MTL::CommandQueue* queue);
        void Unregister();

        /* 设备与命令队列，渲染后端未初始化时返回 nullptr */
        MTL::Device* Device();
        MTL::CommandQueue* Queue();

        /* 当前处于活动状态的渲染命令编码器，无活动 Pass 时返回 nullptr */
        MTL::RenderCommandEncoder* Encoder();
        void SetEncoder(MTL::RenderCommandEncoder* encoder);

        /* 设备是否可用 */
        bool IsAvailable();

        /* 当前帧序号（0 .. MaxFramesInFlight-1）。按帧切片的资源据此选取切片，
         * 由 RenderAPI 在每帧结束时推进。 */
        uint32_t FrameIndex();
        void AdvanceFrame();

        /* ---------- 管线编译产物缓存（MTLBinaryArchive） ----------
         * 「着色器 × 渲染目标格式」的管线第一次创建要把 MSL 编译成 GPU 二进制；产物缓存到磁盘后，
         * 后续进程就能直接命中。缓存文件缺失 / 损坏时自动回退成空缓存。 */

        /* 缓存实例（懒加载；设备不可用时返回 nullptr） */
        MTL::BinaryArchive* PipelineArchive();
        /* 把「真正新编译（未命中缓存）」的管线条目写回缓存；命中缓存的条目不要传入
         * ——重复添加会在缓存里堆积冗余记录。落盘在后续调用 / 退出时按静置窗口触发 */
        void NotifyPipelineCompiled(MTL::RenderPipelineDescriptor* descriptor);
        /* 收尾时把未落盘的条目写出（由 Unregister 统一调用） */
        void FlushPipelineArchive();
    }

    /* 64 位哈希合并（boost::hash_combine 风格）。用于把 VertexDescriptor、
     * 像素格式等结构化信息折叠成一个稳定的缓存键。 */
    inline void MetalHashCombine(uint64_t& seed, uint64_t value)
    {
        seed ^= value + 0x9E3779B97F4A7C15ULL + (seed << 6) + (seed >> 2);
    }

    /* Metal 错误检查宏：记录错误并通过断言在调试期立即暴露问题 */
    #define METAL_CHECK_ERROR(result, msg) \
        do { \
            NS::Error* _metal_err = (result); \
            if (_metal_err) { \
                CORE_LOG_ERROR("Metal Error: {} - {}", msg, \
                    _metal_err->localizedDescription() \
                        ? _metal_err->localizedDescription()->utf8String() : "unknown"); \
                ASSERT(false, "Metal API call failed"); \
            } \
        } while (0)

    /* Metal 对象创建检查宏 */
    #define METAL_CHECK_OBJECT(obj, msg) \
        do { \
            if (!(obj)) { \
                CORE_LOG_ERROR("Metal Error: Failed to create object - {}", msg); \
                ASSERT(false, "Metal object creation failed"); \
            } \
        } while (0)
}

#endif /* PLATFORM_MACOS */
