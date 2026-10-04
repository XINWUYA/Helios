#ifdef PLATFORM_MACOS

#include "Pch.h"
#include "MetalCommon.h"

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
    }
}

#endif /* PLATFORM_MACOS */
