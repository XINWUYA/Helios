#ifdef PLATFORM_MACOS

#include "Pch.h"
#include "MetalWindow.h"
#include "MetalRenderAPI.h"
#include <GLFW/glfw3.h>
#include <objc/message.h>
#include <objc/objc.h>
#include "Helios/Events/ApplicationEvent.h"
#include "Helios/Events/KeyEvent.h"
#include "Helios/Events/MouseEvent.h"
#include "Helios/Renderer/Renderer.h"

/* 前向声明GLFW native函数 */
extern "C" void* glfwGetCocoaWindow(GLFWwindow* window);

/* CALayer 自动缩放掩码（与 kCALayerWidthSizable / kCALayerHeightSizable 同值，
 * 这里不引入 AppKit 头文件，避免在纯 C++ 单元里拉入 ObjC 依赖） */
namespace
{
    constexpr unsigned int kLayerWidthSizable = 1u << 1;
    constexpr unsigned int kLayerHeightSizable = 1u << 4;
}

namespace Helios
{
    /* MetalContext实现 */
    MetalContext::MetalContext(GLFWwindow* window)
        : m_Window(window)
    {
        ASSERT(window, "GLFWwindow is nullptr!");
    }

    MetalContext::~MetalContext()
    {
        if (m_RenderPassDescriptor)
            m_RenderPassDescriptor->release();
        if (m_DepthTexture)
            m_DepthTexture->release();
        /* m_Device 和 m_CommandQueue 由 MetalRenderAPI 拥有，不在此处释放 */
    }

    void MetalContext::Init()
    {
        CreateMetalLayer(m_Window);

        auto metalRenderAPI = std::dynamic_pointer_cast<MetalRenderAPI>(Renderer::GetRenderAPI());
        if (!metalRenderAPI)
        {
            CORE_LOG_ERROR("MetalRenderAPI is not available!");
            return;
        }

        CORE_LOG_INFO("Using Metal:");
        CORE_LOG_INFO("    Device: {}", metalRenderAPI->GetDevice()->name()->utf8String());

        /* 将MetalLayer和RenderPassDescriptor传递给MetalRenderAPI */
        metalRenderAPI->SetMetalLayer(m_MetalLayer);
        metalRenderAPI->SetRenderPassDescriptor(m_RenderPassDescriptor);
    }

    void MetalContext::CreateMetalLayer(GLFWwindow* window)
    {
        auto metalRenderAPI = std::dynamic_pointer_cast<MetalRenderAPI>(Renderer::GetRenderAPI());
        if (!metalRenderAPI)
        {
            CORE_LOG_ERROR("MetalRenderAPI is not available when creating MetalLayer!");
            return;
        }

        /* 使用MetalRenderAPI的设备与命令队列，确保后续渲染使用同一设备 */
        m_Device = metalRenderAPI->GetDevice();
        m_CommandQueue = metalRenderAPI->GetCommandQueue();

        /* 创建Metal图层 */
        m_MetalLayer = CA::MetalLayer::layer();
        m_MetalLayer->setDevice(m_Device);
        m_MetalLayer->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
        m_MetalLayer->setFramebufferOnly(true);

        /* 获取NSWindow并设置Metal图层 - 使用Objective-C运行时 */
        id cocoaWindow = (id)glfwGetCocoaWindow(window);
        id contentView = ((id (*)(id, SEL))objc_msgSend)(cocoaWindow, sel_registerName("contentView"));
        id layer = (id)m_MetalLayer;

        /* layer-hosting 视图，AppKit 不维护 layer 几何；声明宽高可缩放，
         * 配合 SyncLayerGeometry() 使 layer 铺满 contentView。 */
        ((void (*)(id, SEL, unsigned int))objc_msgSend)(layer, sel_registerName("setAutoresizingMask:"),
            kLayerWidthSizable | kLayerHeightSizable);

        ((void (*)(id, SEL, id))objc_msgSend)(contentView, sel_registerName("setLayer:"), layer);
        ((void (*)(id, SEL, BOOL))objc_msgSend)(contentView, sel_registerName("setWantsLayer:"), YES);

        /* contentsScale 与 NSWindow 的 backingScaleFactor 对齐（见 SyncLayerGeometry），
         * 避免 Retina 上 drawable 被降采样。 */
        SyncLayerGeometry();

        /* drawable 尺寸必须等于帧缓冲的物理像素尺寸，
         * 否则会被窗口服务器拉伸到 layer 的 bounds 上。 */
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        m_MetalLayer->setDrawableSize({static_cast<float>(width), static_cast<float>(height)});

        /* 创建渲染通道描述符 */
        m_RenderPassDescriptor = MTL::RenderPassDescriptor::alloc()->init();

        /* 创建深度+模板纹理（尺寸与 drawable 严格一致） */
        RecreateDepthTexture(static_cast<uint32_t>(width), static_cast<uint32_t>(height));

        const double contents_scale = ((double (*)(id, SEL))objc_msgSend)(
            (id)m_MetalLayer, sel_registerName("contentsScale"));
        CORE_LOG_INFO("Metal layer created ({}x{} pixels, contentsScale {})", width, height, contents_scale);
    }

    /* MetalLayer 是 layer-hosting 视图的根层，AppKit 不更新其 contentsScale 与 frame；
     * 每次窗口尺寸/缩放变化时重新对齐，避免 drawable 被降采样或整体拉伸。 */
    void MetalContext::SyncLayerGeometry()
    {
        if (!m_MetalLayer || !m_Window)
            return;

        id cocoaWindow = (id)glfwGetCocoaWindow(m_Window);
        if (!cocoaWindow)
            return;

        id contentView = ((id (*)(id, SEL))objc_msgSend)(cocoaWindow, sel_registerName("contentView"));
        if (!contentView)
            return;

        const CGRect bounds = ((CGRect (*)(id, SEL))objc_msgSend)(contentView, sel_registerName("bounds"));
        const double backing_scale = ((double (*)(id, SEL))objc_msgSend)(cocoaWindow, sel_registerName("backingScaleFactor"));
        if (backing_scale <= 0.0 || bounds.size.width <= 0.0 || bounds.size.height <= 0.0)
            return;

        id layer = (id)m_MetalLayer;
        ((void (*)(id, SEL, double))objc_msgSend)(layer, sel_registerName("setContentsScale:"), backing_scale);

        /* 根层没有 superlayer，frame 取 (0,0) + contentView 大小 */
        const CGRect frame = CGRectMake(0.0, 0.0, bounds.size.width, bounds.size.height);
        ((void (*)(id, SEL, CGRect))objc_msgSend)(layer, sel_registerName("setFrame:"), frame);
    }

    /* 深度+模板附件必须与 drawable 同尺寸，否则 RenderPass 会因附件尺寸不一致而失败，
     * 或造成深度测试区域与颜色区域错位。 */
    void MetalContext::RecreateDepthTexture(uint32_t width, uint32_t height)
    {
        if (!m_Device || !m_RenderPassDescriptor || width == 0 || height == 0)
            return;

        if (m_DepthTexture && m_DepthWidth == width && m_DepthHeight == height)
            return;

        if (m_DepthTexture)
        {
            m_DepthTexture->release();
            m_DepthTexture = nullptr;
        }

        /* 注意：texture2DDescriptor 走的是 ObjC 类方法、返回 autoreleased(+0) 对象，不能手工 release
         * —— 多释放一次会立刻析构，随后 pool 排空再发 release → EXC_BAD_ACCESS（窗口缩放路径就崩在
         * glfwPollEvents 的 pool 里）。 */
        MTL::TextureDescriptor* depthDesc = MTL::TextureDescriptor::texture2DDescriptor(
            MTL::PixelFormatDepth32Float_Stencil8, width, height, false);
        depthDesc->setStorageMode(MTL::StorageModePrivate);
        depthDesc->setUsage(MTL::TextureUsageRenderTarget);
        m_DepthTexture = m_Device->newTexture(depthDesc);

        m_DepthWidth = width;
        m_DepthHeight = height;

        auto depthAttachment = m_RenderPassDescriptor->depthAttachment();
        auto stencilAttachment = m_RenderPassDescriptor->stencilAttachment();

        /* 设置深度附件 */
        depthAttachment->setTexture(m_DepthTexture);
        depthAttachment->setLoadAction(MTL::LoadActionClear);
        depthAttachment->setStoreAction(MTL::StoreActionStore);
        /* Reversed-Z：远平面深度为 0，清零取 0 */
        depthAttachment->setClearDepth(0.0);

        /* 设置模板附件（与深度附件共享同一纹理） */
        stencilAttachment->setTexture(m_DepthTexture);
        stencilAttachment->setLoadAction(MTL::LoadActionClear);
        stencilAttachment->setStoreAction(MTL::StoreActionStore);
        stencilAttachment->setClearStencil(0);
    }

    void MetalContext::OnResize(uint32_t width, uint32_t height)
    {
        /* InitGraphicsContext() 之前 MetalLayer 尚未创建，忽略即可；
         * 实际尺寸由 Init() 读取当前帧缓冲尺寸得到。 */
        if (!m_MetalLayer)
            return;

        SyncLayerGeometry();

        /* 以 GLFW 的帧缓冲尺寸为准，保证与 viewport、事件里派发的尺寸三者一致 */
        int framebuffer_width = static_cast<int>(width);
        int framebuffer_height = static_cast<int>(height);
        glfwGetFramebufferSize(m_Window, &framebuffer_width, &framebuffer_height);
        if (framebuffer_width <= 0 || framebuffer_height <= 0)
            return;

        m_MetalLayer->setDrawableSize({
            static_cast<float>(framebuffer_width),
            static_cast<float>(framebuffer_height) });

        RecreateDepthTexture(static_cast<uint32_t>(framebuffer_width), static_cast<uint32_t>(framebuffer_height));
    }

    void MetalContext::SwapBuffers()
    {
        /* 提交命令缓冲区并呈现drawable */
        auto metalRenderAPI = std::dynamic_pointer_cast<MetalRenderAPI>(Renderer::GetRenderAPI());
        if (metalRenderAPI)
        {
            metalRenderAPI->Present();
        }
    }

    /* MetalWindow实现 */
    MetalWindow::MetalWindow(const WindowDesc& desc)
    {
        Build(desc);
    }

    MetalWindow::~MetalWindow()
    {
        Destroy();
    }

    void MetalWindow::OnUpdate()
    {
        glfwPollEvents();
        
        /* 提交命令缓冲区并交换缓冲区 */
        m_pMetalContext->SwapBuffers();
    }

    void MetalWindow::SetVSync(bool enable)
    {
        m_WindowInfo.Descriptor.IsVSync = enable;
        glfwSwapInterval(enable ? 1 : 0);
    }

    void MetalWindow::Build(const WindowDesc& desc)
    {
        /* 初始化GLFW */
        if (!glfwInit())
        {
            CORE_LOG_ERROR("Could not initialize GLFW!");
            return;
        }

        /* 配置GLFW不创建OpenGL上下文 */
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        glfwWindowHint(GLFW_MAXIMIZED, desc.IsMaximized ? GLFW_TRUE : GLFW_FALSE);

        /* 以主监视器缩放比将目标像素换算为 GLFW 逻辑点：点尺寸 = round(像素 / scale)，
         * 使帧缓冲得到请求的像素尺寸。 */
        float monitor_scale_x = 1.0f;
        float monitor_scale_y = 1.0f;
        if (GLFWmonitor* monitor = glfwGetPrimaryMonitor())
            glfwGetMonitorContentScale(monitor, &monitor_scale_x, &monitor_scale_y);
        if (monitor_scale_x <= 0.0f) monitor_scale_x = 1.0f;
        if (monitor_scale_y <= 0.0f) monitor_scale_y = 1.0f;

        const int window_width = static_cast<int>(desc.Width / monitor_scale_x + 0.5f);
        const int window_height = static_cast<int>(desc.Height / monitor_scale_y + 0.5f);

        m_pGLFWWindow = glfwCreateWindow(
            window_width < 1 ? 1 : window_width,
            window_height < 1 ? 1 : window_height,
            desc.Title.c_str(), nullptr, nullptr);
        if (!m_pGLFWWindow)
        {
            CORE_LOG_ERROR("Could not create GLFW window!");
            glfwTerminate();
            return;
        }

        /* 引擎只保留"物理像素"一套尺寸，回写帧缓冲实际值（可能被窗口服务器夹取） */
        int framebuffer_width = 0;
        int framebuffer_height = 0;
        glfwGetFramebufferSize(m_pGLFWWindow, &framebuffer_width, &framebuffer_height);

        float scale_x = 1.0f;
        float scale_y = 1.0f;
        glfwGetWindowContentScale(m_pGLFWWindow, &scale_x, &scale_y);

        m_WindowInfo.Descriptor = desc;
        if (framebuffer_width > 0 && framebuffer_height > 0)
        {
            m_WindowInfo.Descriptor.Width = static_cast<uint32_t>(framebuffer_width);
            m_WindowInfo.Descriptor.Height = static_cast<uint32_t>(framebuffer_height);
        }
        if (!desc.IsMaximized && (m_WindowInfo.Descriptor.Width != desc.Width || m_WindowInfo.Descriptor.Height != desc.Height))
        {
            /* 窗口服务器可能将窗口约束到屏幕可视区，请求分辨率无法满足时告警 */
            CORE_LOG_WARN("Requested resolution {}x{} pixels was clamped to {}x{} pixels "
                "(screen working area is smaller).", desc.Width, desc.Height,
                m_WindowInfo.Descriptor.Width, m_WindowInfo.Descriptor.Height);
        }
        m_WindowInfo.ContentScale = (scale_x > 0.0f) ? scale_x : 1.0f;
        m_WindowInfo.Owner = this;

        glfwSetWindowUserPointer(m_pGLFWWindow, &m_WindowInfo);
        SetVSync(desc.IsVSync);

        /* 窗口最小尺寸（点）：给停靠布局一个物理下限。ImGui DockNode 的最小尺寸取自 WindowMinSize，
         * 三栏布局最小宽 ≈ 600 点，取 800×480 既托得住布局、又低于默认的 960×540；不然 DockNode 被
         * 钳制会丢比例。 */
        {
            constexpr int kMinWindowWidthPoints = 800;
            constexpr int kMinWindowHeightPoints = 480;
            glfwSetWindowSizeLimits(m_pGLFWWindow,
                kMinWindowWidthPoints, kMinWindowHeightPoints,
                GLFW_DONT_CARE, GLFW_DONT_CARE);
        }

        /* 创建 Metal 上下文；实际初始化由 Renderer::Init() 后的 InitGraphicsContext() 触发 */
        m_pMetalContext = CreateUniquePtr<MetalContext>(m_pGLFWWindow);

        /* 帧缓冲回调：物理像素。渲染分辨率（drawable / depth / viewport）以它为准 */
        glfwSetFramebufferSizeCallback(m_pGLFWWindow, [](GLFWwindow* window, int width, int height) {
            WindowInfo* info = (WindowInfo*)glfwGetWindowUserPointer(window);
            if (!info)
                return;

            if (info->Owner)
                info->Owner->OnFramebufferSizeChanged(width, height);
        });

        /* 最小化/还原回调：显式上报（最小化时帧缓冲为 0×0，但上面会忽略它，
         * 因此不能靠 0 尺寸推断最小化） */
        glfwSetWindowIconifyCallback(m_pGLFWWindow, [](GLFWwindow* window, int iconified) {
            WindowInfo* info = (WindowInfo*)glfwGetWindowUserPointer(window);
            if (!info)
                return;

            if (info->Owner)
                info->Owner->OnIconified(iconified == GLFW_TRUE);
        });

        /* 内容缩放回调：窗口在不同 DPI 屏幕间移动，或系统缩放变化时触发 */
        glfwSetWindowContentScaleCallback(m_pGLFWWindow, [](GLFWwindow* window, float x_scale, float y_scale) {
            WindowInfo* info = (WindowInfo*)glfwGetWindowUserPointer(window);
            if (!info)
                return;

            if (info->Owner)
                info->Owner->OnContentScaleChanged(x_scale, y_scale);
        });

        /* 设置窗口关闭回调 */
        glfwSetWindowCloseCallback(m_pGLFWWindow, [](GLFWwindow* window) {
            WindowInfo* info = (WindowInfo*)glfwGetWindowUserPointer(window);
            WindowCloseEvent event;
            info->CallBackFunc(&event);
        });

        /* 设置键盘回调 */
        glfwSetKeyCallback(m_pGLFWWindow, [](GLFWwindow* window, int key, int scancode, int action, int mods) {
            WindowInfo* info = (WindowInfo*)glfwGetWindowUserPointer(window);
            switch (action) {
                case GLFW_PRESS: {
                    KeyPressedEvent event(key, 0);
                    info->CallBackFunc(&event);
                    break;
                }
                case GLFW_RELEASE: {
                    KeyReleasedEvent event(key);
                    info->CallBackFunc(&event);
                    break;
                }
                case GLFW_REPEAT: {
                    KeyPressedEvent event(key, 1);
                    info->CallBackFunc(&event);
                    break;
                }
            }
        });

        /* 设置鼠标按钮回调 */
        glfwSetMouseButtonCallback(m_pGLFWWindow, [](GLFWwindow* window, int button, int action, int mods) {
            WindowInfo* info = (WindowInfo*)glfwGetWindowUserPointer(window);
            switch (action) {
                case GLFW_PRESS: {
                    MouseButtonPressedEvent event(button);
                    info->CallBackFunc(&event);
                    break;
                }
                case GLFW_RELEASE: {
                    MouseButtonReleasedEvent event(button);
                    info->CallBackFunc(&event);
                    break;
                }
            }
        });

        /* 设置滚轮回调 */
        glfwSetScrollCallback(m_pGLFWWindow, [](GLFWwindow* window, double xOffset, double yOffset) {
            WindowInfo* info = (WindowInfo*)glfwGetWindowUserPointer(window);
            MouseScrolledEvent event((float)xOffset, (float)yOffset);
            info->CallBackFunc(&event);
        });

        /* 设置光标位置回调 */
        glfwSetCursorPosCallback(m_pGLFWWindow, [](GLFWwindow* window, double xPos, double yPos) {
            WindowInfo* info = (WindowInfo*)glfwGetWindowUserPointer(window);
            MouseMovedEvent event((float)xPos, (float)yPos);
            info->CallBackFunc(&event);
        });

        CORE_LOG_INFO("Metal window created: {} ({}x{} pixels, content scale {})",
            m_WindowInfo.Descriptor.Title, m_WindowInfo.Descriptor.Width, m_WindowInfo.Descriptor.Height,
            m_WindowInfo.ContentScale);
    }

    /* 物理像素变化：同步 drawable / 深度纹理后派发 resize 事件；
     * 事件尺寸为物理像素，Application 据此设置 viewport，RenderTarget 也按它分配。 */
    void MetalWindow::OnFramebufferSizeChanged(int width, int height)
    {
        if (width <= 0 || height <= 0)
            return;

        m_WindowInfo.Descriptor.Width = static_cast<uint32_t>(width);
        m_WindowInfo.Descriptor.Height = static_cast<uint32_t>(height);

        if (m_pMetalContext)
            m_pMetalContext->OnResize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));

        WindowResizeEvent event(static_cast<unsigned int>(width), static_cast<unsigned int>(height));
        m_WindowInfo.CallBackFunc(&event);
    }

    /* 内容缩放变化：像素/点比例变了，即便点数没变也要重建 drawable */
    void MetalWindow::OnContentScaleChanged(float scale_x, float scale_y)
    {
        (void)scale_y;
        m_WindowInfo.ContentScale = (scale_x > 0.0f) ? scale_x : 1.0f;

        if (m_pMetalContext)
        {
            m_pMetalContext->OnResize(m_WindowInfo.Descriptor.Width, m_WindowInfo.Descriptor.Height);
        }
    }

    /* 最小化/还原：派发显式事件，由 Application 维护 m_IsMinimized */
    void MetalWindow::OnIconified(bool iconified)
    {
        WindowIconifyEvent event(iconified);
        m_WindowInfo.CallBackFunc(&event);
    }

    void MetalWindow::Destroy()
    {
        glfwDestroyWindow(m_pGLFWWindow);
    }
}

#endif
