#pragma once

#ifdef PLATFORM_MACOS

#include "Helios/VirtualDevice/DeviceWindow.h"
#include "Helios/VirtualDevice/DeviceContext.h"
#include <QuartzCore/CAMetalLayer.hpp>
#include <Metal/Metal.hpp>

struct GLFWwindow;

namespace Helios
{
    /* MetalContext类：管理Metal渲染上下文和交换链 */
    class MetalContext final : public DeviceContext
    {
    public:
        MetalContext(GLFWwindow* window);
        ~MetalContext() override;

        void Init() override;
        void SwapBuffers() override;

        /* 窗口尺寸变化：同步 MetalLayer 几何（contentsScale / frame / drawableSize）
         * 并按新尺寸重建深度附件。width/height 为物理像素。 */
        void OnResize(uint32_t width, uint32_t height);

        /* Metal特有接口 */
        MTL::Device* GetDevice() const { return m_Device; }
        MTL::CommandQueue* GetCommandQueue() const { return m_CommandQueue; }
        CA::MetalLayer* GetMetalLayer() const { return m_MetalLayer; }
        MTL::RenderPassDescriptor* GetRenderPassDescriptor() const { return m_RenderPassDescriptor; }
        MTL::Drawable* GetCurrentDrawable() const { return m_CurrentDrawable; }

    private:
        void CreateMetalLayer(GLFWwindow* window);
        /* 把 layer 的 contentsScale / frame 与 NSView 对齐，避免 drawable 被降采样 */
        void SyncLayerGeometry();
        /* 按 width x height（物理像素）重建深度+模板纹理 */
        void RecreateDepthTexture(uint32_t width, uint32_t height);

        GLFWwindow* m_Window{ nullptr };
        uint32_t m_DepthWidth{ 0 };
        uint32_t m_DepthHeight{ 0 };
        MTL::Device* m_Device{ nullptr };
        MTL::CommandQueue* m_CommandQueue{ nullptr };
        CA::MetalLayer* m_MetalLayer{ nullptr };
        MTL::RenderPassDescriptor* m_RenderPassDescriptor{ nullptr };
        MTL::Drawable* m_CurrentDrawable{ nullptr };
        MTL::Texture* m_DepthTexture{ nullptr };
    };

    /* MetalWindow类：使用GLFWwindow创建Metal窗口 */
    class MetalWindow : public DeviceWindow
    {
    public:
        MetalWindow(const WindowDesc& desc);
        ~MetalWindow() override;

        void OnUpdate() override;
        /* 物理像素 */
        [[nodiscard]] uint32_t GetWidth() const override { return m_WindowInfo.Descriptor.Width; }
        [[nodiscard]] uint32_t GetHeight() const override { return m_WindowInfo.Descriptor.Height; }
        [[nodiscard]] float GetContentScale() const override { return m_WindowInfo.ContentScale; }
        [[nodiscard]] bool IsVSync() const override { return m_WindowInfo.Descriptor.IsVSync; }
        void SetVSync(bool enable) override;
        [[nodiscard]] void* GetNativeWindow() const override { return m_pGLFWWindow; }
        void SetEventCallback(const EventCallbackFunc& callback) override { m_WindowInfo.CallBackFunc = callback; }
        MetalContext* GetMetalContext() const { return m_pMetalContext.get(); }
        void InitGraphicsContext() override { m_pMetalContext->Init(); }

    private:
        void Build(const WindowDesc& desc);
        void Destroy();

        /* GLFW 回调转发（需要在静态回调里访问 this 与 Metal 上下文） */
        void OnFramebufferSizeChanged(int width, int height);
        void OnContentScaleChanged(float scale_x, float scale_y);
        /* 最小化/还原：显式上报最小化状态（不依赖 0×0 帧缓冲推断） */
        void OnIconified(bool iconified);

        struct WindowInfo
        {
            WindowDesc Descriptor;                  /* 物理像素 */
            float ContentScale{ 1.0f };             /* 像素 / 点 */
            class MetalWindow* Owner{ nullptr };    /* 供静态 GLFW 回调回指窗口实例 */
            EventCallbackFunc CallBackFunc;
        };

        GLFWwindow* m_pGLFWWindow{ nullptr };
        WindowInfo m_WindowInfo{};
        UniquePtr<MetalContext> m_pMetalContext{ nullptr };
    };
}

#endif
