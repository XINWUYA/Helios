#pragma once

#include "Helios/VirtualDevice/DeviceShader.h"
#include "Helios/VirtualDevice/DeviceTexture.h"
#include "Helios/VirtualDevice/DeviceBuffer.h"
#include "Helios/VirtualDevice/DeviceVertexArray.h"
#include "Helios/VirtualDevice/DeviceUniformBuffer.h"
#include <glm/glm.hpp>
#include <vector>
#include <cstdint>

// Forward declarations for ImGui types
struct ImDrawData;
struct ImDrawList;

namespace Helios
{
    /**
     * \brief ImGui渲染器 - 类似Filament的filagui设计
     * 不使用ImGui官方后端，而是通过Helios的RHI抽象层实现跨平台渲染
     */
    class ImGuiRenderer
    {
    public:
        ImGuiRenderer();
        ~ImGuiRenderer();

        /* 初始化渲染器 */
        void Init();
        /* 释放资源 */
        void Cleanup();

        /* 开始新帧 - 更便显示尺寸等 */
        void NewFrame();
        /* 处理ImGui绘制数据并渲染 */
        void RenderDrawData();

        /* 设置显示尺寸 */
        void SetDisplaySize(int width, int height, float scale_x = 1.0f, float scale_y = 1.0f);

        /* 获取字体纹理 */
        SharedPtr<DeviceTexture> GetFontTexture() const { return m_FontTexture; }

        /* 重建字体图集纹理：窗口在 DPI 不同的显示器之间移动时，字体需按新的 content scale
         * 重新光栅化（否则文字发虚 / 过小）。调用前须先重建 ImGui 字体图集（StyleInstaller），
         * 本方法只负责把新图集数据上传成 GPU 纹理并更新 io.Fonts->TexID。
         * 释放旧纹理 → 重新取图集 → 重传。 */
        void RebuildFontTexture();

    private:
        /* 创建字体图集纹理 */
        void CreateFontTexture();
        /* 创建UI着色器 */
        void CreateUIShader();
        /* 创建深度变体着色器（深度纹理预览：阴影图 / GBuffer 深度按 depth2d 采样） */
        void CreateDepthShader();
        /* 创建缓冲区 */
        void EnsureBuffersCapacity(int vertex_count, int index_count);
        /* 更新投影矩阵 */
        void UpdateProjectionMatrix();

        /* 显示尺寸 */
        int m_DisplayWidth{ 0 };
        int m_DisplayHeight{ 0 };
        float m_ScaleX{ 1.0f };
        float m_ScaleY{ 1.0f };

        /* 字体纹理 */
        SharedPtr<DeviceTexture> m_FontTexture{ nullptr };

        /* UI着色器 */
        SharedPtr<DeviceShader> m_UIShader{ nullptr };
        /* 深度变体着色器：深度格式纹理（深度附件预览）按 depth2d 采样、灰度输出 */
        SharedPtr<DeviceShader> m_UIDepthShader{ nullptr };

        /* 顶点 / 索引缓冲：三槽轮转 —— GPU 在飞帧仍读旧槽，CPU 只覆写下一槽才安全
         *（对齐 ImGui 官方 Metal 后端的 frames-in-flight；drawable 池天然限流 ≤3 帧在飞）。
         * 别改成单槽省内存：会偶现顶点 / UV 错乱（拉丝、把字体图集采样出来）。 */
        static constexpr int kFramesInFlight = 3;
        SharedPtr<DeviceVertexBuffer> m_VertexBuffers[kFramesInFlight]{};
        SharedPtr<IndexBuffer> m_IndexBuffers[kFramesInFlight]{};
        SharedPtr<DeviceVertexArray> m_VertexArrays[kFramesInFlight]{};
        int m_FrameSlot{ 0 };

        /* 缓冲区容量（槽间共享：扩容时一次性重建全部槽 —— 扩容极少发生） */
        int m_VertexBufferSize{ 0 };
        int m_IndexBufferSize{ 0 };

        /* 每帧复用的CPU暂存缓冲（以字节为单位，避免依赖ImGui类型前向声明） */
        std::vector<uint8_t> m_VtxScratch;
        std::vector<uint8_t> m_IdxScratch;

        /* UI uniform buffer */
        SharedPtr<DeviceUniformBuffer> m_UIUniformBuffer{ nullptr };

        /* 投影矩阵 */
        glm::mat4 m_ProjectionMatrix{ 1.0f };

        /* 诊断（临时）：上一帧的"帧图绑定"计数；只在与本帧不同时打日志（稳态静默）。
         * 0 = 起始假设"无图"，首帧无图不打；见 RenderDrawData 末尾 */
        int m_DebugLastFrameGraphBinds{ 0 };
    };
}
