#pragma once

#ifdef PLATFORM_MACOS

#include "Helios/Renderer/RenderAPI.h"
#include "MetalShader.h"
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>
#include <unordered_map>

namespace Helios
{
    class MetalRenderAPI : public RenderAPI
    {
    public:
        ~MetalRenderAPI() override;

        void Init() override;
        void SetViewport(uint32_t x_start, uint32_t y_start, uint32_t width, uint32_t height) override;
        void SetScissor(uint32_t x, uint32_t y, uint32_t width, uint32_t height) override;
        void SetClearColor(const glm::vec4& color) override;
        void Clear() override;

        /* 记录光栅化状态并应用到当前编码器 */
        void ApplyRasterState(RenderRasterState raster_state) override;

        void DrawIndexed(PrimitiveType type, const SharedPtr<DeviceVertexArray>& vertex_array,
            uint32_t index_count = 0, uint32_t index_offset = 0) override;
        void DrawArrays(PrimitiveType type, const SharedPtr<DeviceVertexArray>& vertex_array) override;

        void Flush() override;

        /* 在当前帧的命令缓冲区上插入 blit 编码器生成 mipmap（见 RenderAPI 的说明） */
        void GenerateMipmap(const SharedPtr<DeviceTexture>& texture) override;

        /* 呈现当前帧 */
        void Present();

        void PushDebugGroup(const char* name) override;
        void PopDebugGroup() override;

        /* ---------------- Metal 特有接口 ---------------- */
        MTL::Device* GetDevice() const { return m_Device; }
        MTL::CommandQueue* GetCommandQueue() const { return m_CommandQueue; }
        MTL::CommandBuffer* GetCurrentCommandBuffer() const { return m_CurrentCommandBuffer; }
        MTL::RenderCommandEncoder* GetCurrentRenderEncoder() const { return m_CurrentRenderEncoder; }
        CA::MetalDrawable* GetCurrentDrawable() const { return m_CurrentDrawable; }

        void SetMetalLayer(CA::MetalLayer* layer) { m_MetalLayer = layer; }
        void SetRenderPassDescriptor(MTL::RenderPassDescriptor* descriptor) { m_RenderPassDescriptor = descriptor; }
        CA::MetalLayer* GetMetalLayer() const { return m_MetalLayer; }
        MTL::RenderPassDescriptor* GetRenderPassDescriptor() const { return m_RenderPassDescriptor; }
        MTL::RenderPassDescriptor* GetActiveRenderPassDescriptor() const { return m_ActiveRenderPassDescriptor; }

        /* 渲染通道管理 */
        void BeginRenderPass(MTL::RenderPassDescriptor* renderPassDescriptor);
        void EndRenderPass();

        void BeginDefaultRenderPass(bool preserve_content) override;
        void EndDefaultRenderPass() override;

        /* 准备本帧的 drawable（RenderAPI::PrepareNextFrame 的 Metal 实现） */
        void PrepareNextFrame() override;
        void PrepareNextDrawable();

        uint32_t GetCurrentWidth() const { return m_CurrentWidth; }
        uint32_t GetCurrentHeight() const { return m_CurrentHeight; }

        /* 默认渲染目标的颜色格式，供无法从 Pass 描述符推断的场合使用 */
        MTL::PixelFormat GetDefaultColorFormat() const { return m_DefaultColorFormat; }

        /* 由 MetalShader::Bind 登记当前着色器。
         * 管线状态必须在绘制时依据光栅状态与当前 Pass 的附件格式创建，
         * 因此这里只做登记，实际状态在 ApplyPipelineState 中落地。 */
        void SetBoundShader(MetalShader* shader) { m_BoundShader = shader; }
        MetalShader* GetBoundShader() const { return m_BoundShader; }

        /* 依据当前 RenderPass 附件格式、采样数与光栅状态构建管线描述 */
        [[nodiscard]] MetalPipelineDesc BuildPipelineDesc(uint64_t vertex_layout_hash) const;

    private:
        /* 依据深度比较方式/深度写入开关获取（并缓存）DepthStencilState */
        MTL::DepthStencilState* GetOrCreateDepthStencilState(CompareFunc compare_func, bool depth_write_enabled);

        /* 当前激活的 RenderPass 是否带有深度附件（无深度附件时不能设置深度测试） */
        bool ActiveRenderPassHasDepthAttachment() const;

        /* 把视口下发到当前编码器。渲染到离屏纹理时会翻转 y 轴，
         * 使离屏纹理的行序与 OpenGL 一致（详见实现处的说明）。 */
        void ApplyViewport(uint32_t x, uint32_t y, uint32_t width, uint32_t height);

        /* 把剔除模式、正面朝向与深度状态应用到当前编码器。
         * 这些状态保存在编码器/深度模板状态上而非管线对象中，因此在切换
         * RenderPass 或着色器后都需要重新应用。 */
        void ApplyEncoderRasterState();

        /* 在绘制前解析并设置管线状态、深度状态与材质参数 */
        void ApplyPipelineState(uint64_t vertex_layout_hash);

        /* 把当前清除色写入默认渲染目标的颜色附件。
         * 必须在 Pass 开始时而非 PrepareNextDrawable 时调用：SetClearColor 通常
         * 在本帧 PrepareNextDrawable 之后才被调用，沿用旧值会导致清除滞后一帧。 */
        void ApplyDefaultClearColor();

        MTL::Device* m_Device{ nullptr };
        MTL::CommandQueue* m_CommandQueue{ nullptr };

        CA::MetalLayer* m_MetalLayer{ nullptr };
        MTL::RenderPassDescriptor* m_RenderPassDescriptor{ nullptr };
        MTL::RenderPassDescriptor* m_ActiveRenderPassDescriptor{ nullptr };

        MTL::CommandBuffer* m_CurrentCommandBuffer{ nullptr };
        MTL::RenderCommandEncoder* m_CurrentRenderEncoder{ nullptr };
        CA::MetalDrawable* m_CurrentDrawable{ nullptr };

        /* 帧级 autorelease pool：Metal-CPP 的便捷工厂返回的是 autoreleased 对象、注册进主线程的隐式
         * pool；而引擎渲染循环不经过 AppKit 事件循环、隐式 pool 永远不排空，对象逐帧堆积、内存暴涨。
         * 做法：帧入口建池、Present 提交后排空。 */
        NS::AutoreleasePool* m_FrameAutoreleasePool{ nullptr };

        uint32_t m_CurrentWidth{ 0 };
        uint32_t m_CurrentHeight{ 0 };

        /* 当前 Pass 是否渲染到离屏纹理（false = 渲染到 drawable/屏幕）。
         * 离屏 Pass 需要翻转视口的 y 轴，理由见 ApplyViewport 的实现说明。 */
        bool m_CurrentPassIsOffscreen{ false };

        glm::vec4 m_ClearColor{ 0.0f, 0.0f, 0.0f, 1.0f };

        /* 默认渲染目标的颜色格式，由窗口层在设置图层时确定 */
        MTL::PixelFormat m_DefaultColorFormat{ MTL::PixelFormatBGRA8Unorm };

        RenderRasterState m_CurrentRasterState{};
        MetalShader* m_BoundShader{ nullptr };

        /* DepthStencilState 缓存，键 = (CompareFunc << 1) | depthWriteEnabled */
        std::unordered_map<uint32_t, MTL::DepthStencilState*> m_DepthStencilStates;

        /* 编码器级状态的最近一次应用记录。剔除模式、正面朝向与深度模板状态都
         * 保存在编码器（而非管线对象）上，同一编码器内状态未变时无需重复下发。 */
        MTL::RenderCommandEncoder* m_RasterStateEncoder{ nullptr };
        uint64_t m_AppliedRasterStateHash{ 0 };
        MTL::RenderPipelineState* m_ActivePipelineState{ nullptr };
    };
}

#endif /* PLATFORM_MACOS */
