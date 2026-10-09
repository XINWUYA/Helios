#include "Pch.h"
#include "ImGuiRenderer.h"
#include <imgui.h>

#include "glm/gtc/type_ptr.hpp"
#include "Helios/Renderer/Renderer.h"
#include "Helios/Renderer/RenderAPI.h"

#include <Helios/Scene/SceneCommon.h>

namespace Helios
{
    /* UI uniform data for UBO */
    struct ImGuiUniformData
    {
        glm::mat4 ProjectionMatrix{ 1.0f };
    };

    ImGuiRenderer::ImGuiRenderer()
    {
    }

    ImGuiRenderer::~ImGuiRenderer()
    {
        Cleanup();
    }

    void ImGuiRenderer::Init()
    {
        /* 创建顶点数组 - 必须先创建 */
        m_VertexArray = DeviceVertexArray::Create("ImGui_VertexArray");

        /* 创建初始缓冲区 - 必须在创建着色器之前 */
        EnsureBuffersCapacity(1000, 2000);

        /* 创建UI着色器 - 需要在VertexBuffer设置好布局后创建 */
        CreateUIShader();

        /* 创建深度变体着色器（与 UI 着色器共用顶点布局） */
        CreateDepthShader();

        /* 创建字体纹理 */
        CreateFontTexture();

        /* 创建UI uniform buffer */
        m_UIUniformBuffer = DeviceUniformBuffer::Create(sizeof(ImGuiUniformData), 4);

        CORE_LOG_INFO("ImGuiRenderer initialized successfully");
    }

    void ImGuiRenderer::Cleanup()
    {
        m_VertexArray.reset();
        m_VertexBuffer.reset();
        m_IndexBuffer.reset();
        m_UIShader.reset();
        m_UIDepthShader.reset();
        m_FontTexture.reset();
        m_UIUniformBuffer.reset();
    }

    void ImGuiRenderer::NewFrame()
    {
        /* 更新投影矩阵 - 使用正交投影 */
        UpdateProjectionMatrix();
    }

    void ImGuiRenderer::RenderDrawData()
    {
        ImDrawData* draw_data = ImGui::GetDrawData();

        if (!draw_data || draw_data->DisplaySize.x <= 0 || draw_data->DisplaySize.y <= 0)
            return;

        /* 更新显示尺寸 */
        SetDisplaySize(
            static_cast<int>(draw_data->DisplaySize.x * draw_data->FramebufferScale.x),
            static_cast<int>(draw_data->DisplaySize.y * draw_data->FramebufferScale.y),
            draw_data->FramebufferScale.x,
            draw_data->FramebufferScale.y
        );

        /* 计算总顶点和索引数量 */
        int total_vertex_count = 0;
        int total_index_count = 0;
        for (int n = 0; n < draw_data->CmdListsCount; n++)
        {
            const ImDrawList* draw_list = draw_data->CmdLists[n];
            total_vertex_count += draw_list->VtxBuffer.Size;
            total_index_count += draw_list->IdxBuffer.Size;
        }
        
        if (total_vertex_count == 0 || total_index_count == 0)
            return;

        /* 确保缓冲区足够大 */
        EnsureBuffersCapacity(total_vertex_count, total_index_count);

        /* 填充顶点和索引数据 */
        int vertex_offset = 0;
        int index_offset = 0;

        /* 复用成员级暂存缓冲，避免每帧 new/delete 造成的堆碎片与内存抖动 */
        m_VtxScratch.resize(static_cast<size_t>(total_vertex_count) * sizeof(ImDrawVert));
        m_IdxScratch.resize(static_cast<size_t>(total_index_count) * sizeof(ImDrawIdx));
        auto* vtx_data = reinterpret_cast<ImDrawVert*>(m_VtxScratch.data());
        auto* idx_data = reinterpret_cast<ImDrawIdx*>(m_IdxScratch.data());

        for (int n = 0; n < draw_data->CmdListsCount; n++)
        {
            const ImDrawList* draw_list = draw_data->CmdLists[n];
            
            /* 复制顶点数据 */
            memcpy(vtx_data + vertex_offset, draw_list->VtxBuffer.Data, draw_list->VtxBuffer.Size * sizeof(ImDrawVert));
            
            /* 复制索引数据 - 需要调整索引值，加上顶点偏移 */
            for (int i = 0; i < draw_list->IdxBuffer.Size; i++)
            {
                idx_data[index_offset + i] = draw_list->IdxBuffer.Data[i] + vertex_offset;
            }
            
            vertex_offset += draw_list->VtxBuffer.Size;
            index_offset += draw_list->IdxBuffer.Size;
        }

        /* 上传顶点数据 */
        m_VertexBuffer->SetData(vtx_data, total_vertex_count * sizeof(ImDrawVert));

        /* 上传索引数据 - 容量已在 EnsureBuffersCapacity 中保证足够 */
        m_IndexBuffer->SetData(idx_data, total_index_count * sizeof(ImDrawIdx));

        /* 统一走通用 RenderAPI：Metal 与 OpenGL 共用同一套绘制调用。
         * 平台差异（管线状态与附件格式、纹理/采样器槽位、裁剪 Y 轴方向、索引类型）
         * 全部由各后端在内部消化。 */
        auto renderAPI = Renderer::GetRenderAPI();
        if (!renderAPI)
        {
            CORE_LOG_ERROR("RenderAPI is not available for ImGui rendering");
            return;
        }

        /* 保存并配置渲染状态：ImGui需要的渲染状态（开启混合，关闭深度，关闭剔除） */
        RenderRasterState raster_state;
        raster_state.CullMode = CullMode::Cull_None;
        raster_state.EnableBlend = true;
        raster_state.BlendEquationRGB = BlendEquation::Add;
        raster_state.BlendEquationA = BlendEquation::Add;
        raster_state.BlendFuncSrcRGB = BlendFunc::SrcAlpha;
        raster_state.BlendFuncSrcA = BlendFunc::One;
        raster_state.BlendFuncDstRGB = BlendFunc::OneMinusSrcAlpha;
        raster_state.BlendFuncDstA = BlendFunc::OneMinusSrcAlpha;
        raster_state.EnableDepthWrite = false;
        raster_state.EnableColorWrite = true;
        renderAPI->ApplyRasterState(raster_state);

        /* 设置视口为整个窗口，保证ImGui能绘制到屏幕 */
        renderAPI->SetViewport(0, 0, static_cast<uint32_t>(m_DisplayWidth), static_cast<uint32_t>(m_DisplayHeight));

        /* 绑定着色器、UI uniform buffer 与顶点数组 */
        m_UIShader->Bind();
        /* 字体纹理绑定到纹理槽 0（与 GLSL 中 sampler2D u_FontTexture 的 binding 一致） */
        m_UIShader->SetInt("u_FontTexture", 0);

        /* UIUniformBuffer 在 GLSL 中声明了 binding = 4，由 MetalUniformBuffer/OpenGL
         * 各自的 Bind() 落到后端对应的槽位，调用方无需关心具体索引。 */
        if (m_UIUniformBuffer)
        {
            m_UIUniformBuffer->Bind();
        }

        /* 绑定VAO（顶点/索引缓冲已在上面填充好） */
        m_VertexArray->Bind();

        /* 遍历所有DrawList和DrawCmd，分批次渲染 */
        int local_index_offset = 0;
        ImTextureID last_texture_id = nullptr;
        /* 当前登记的着色器：默认 UI 变体，绘制深度纹理时切到深度变体（见纹理绑定处） */
        DeviceShader* bound_shader = m_UIShader.get();

        for (int n = 0; n < draw_data->CmdListsCount; n++)
        {
            const ImDrawList* draw_list = draw_data->CmdLists[n];

            for (int cmd_i = 0; cmd_i < draw_list->CmdBuffer.Size; cmd_i++)
            {
                const ImDrawCmd* pcmd = &draw_list->CmdBuffer[cmd_i];

                if (pcmd->UserCallback)
                {
                    /* 用户回调 */
                    pcmd->UserCallback(draw_list, pcmd);
                }
                else
                {
                    /* 计算裁剪区域 */
                    ImVec2 clip_off = draw_data->DisplayPos;
                    ImVec2 clip_scale = draw_data->FramebufferScale;

                    ImVec2 clip_min(
                        (pcmd->ClipRect.x - clip_off.x) * clip_scale.x,
                        (pcmd->ClipRect.y - clip_off.y) * clip_scale.y
                    );
                    ImVec2 clip_max(
                        (pcmd->ClipRect.z - clip_off.x) * clip_scale.x,
                        (pcmd->ClipRect.w - clip_off.y) * clip_scale.y
                    );

                    if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y)
                    {
                        local_index_offset += pcmd->ElemCount;
                        continue;
                    }

                    /* 按 OpenGL 约定给出裁剪矩形（原点在左下角）；Y 轴方向由各
                     * 后端在 SetScissor 内部消化，调用方不区分平台。 */
                    uint32_t scissor_x = static_cast<uint32_t>(clip_min.x);
                    uint32_t scissor_y = static_cast<uint32_t>(m_DisplayHeight - clip_max.y);
                    uint32_t scissor_w = static_cast<uint32_t>(clip_max.x - clip_min.x);
                    uint32_t scissor_h = static_cast<uint32_t>(clip_max.y - clip_min.y);
                    renderAPI->SetScissor(scissor_x, scissor_y, scissor_w, scissor_h);

                    /* 绑定纹理：ImTextureID 统一存 DeviceTexture 指针，各后端在自己槽位约定下绑定。别把硬件
                     * 句柄塞进 ImTextureID —— Metal 的纹理句柄是 64 位，而 GetTextureID() 返回 32 位、高位会丢。 */
                    if (pcmd->TextureId != last_texture_id)
                    {
                        DeviceTexture* texture = reinterpret_cast<DeviceTexture*>(pcmd->TextureId);

                        if (!texture)
                        {
                            texture = m_FontTexture.get();
                        }

                        if (texture)
                        {
                            texture->Bind(0);
                        }

                        /* 深度纹理（阴影图 / GBuffer 深度等预览）必须按 depth2d 采样
                         * （深度格式不能绑到 texture2d<float>，见 ImGuiUIDepth.glsl 与
                         * ShaderCompiler 的 @depth-texture 标注）：按纹理格式切着色器 */
                        const bool is_depth = texture != nullptr
                            && IsDepthFormat(texture->GetTextureDesc().Format);
                        DeviceShader* wanted = (is_depth && m_UIDepthShader)
                            ? m_UIDepthShader.get() : m_UIShader.get();
                        if (wanted != nullptr && wanted != bound_shader)
                        {
                            wanted->Bind();
                            if (wanted == m_UIDepthShader.get())
                                wanted->SetInt("u_DepthTexture", 0);
                            bound_shader = wanted;
                        }

                        last_texture_id = pcmd->TextureId;
                    }

                    /* 执行绘制 */
                    renderAPI->DrawIndexed(
                        PrimitiveType::Triangles,
                        m_VertexArray,
                        pcmd->ElemCount,
                        static_cast<uint32_t>(local_index_offset)
                    );
                }

                local_index_offset += pcmd->ElemCount;
            }
        }

        m_VertexArray->Unbind();
        m_UIShader->Unbind();
    }

    void ImGuiRenderer::SetDisplaySize(int width, int height, float scale_x, float scale_y)
    {
        m_DisplayWidth = width;
        m_DisplayHeight = height;
        m_ScaleX = scale_x;
        m_ScaleY = scale_y;
        
        /* 立即更新投影矩阵 - 确保在任何渲染调用前矩阵都是正确的 */
        UpdateProjectionMatrix();
    }

    void ImGuiRenderer::UpdateProjectionMatrix()
    {
        /* 更新投影矩阵 - 使用正交投影 */
        if (m_DisplayWidth > 0 && m_DisplayHeight > 0)
        {
            /* 
             * ImGui uses coordinate system: origin at top-left, Y grows downward
             * NDC/OpenGL expects: origin at center, Y grows upward
             * We need to flip Y axis to correctly map ImGui coordinates to NDC
             */
            float left = 0.0f;
            float right = static_cast<float>(m_DisplayWidth) / m_ScaleX;
            float bottom = static_cast<float>(m_DisplayHeight) / m_ScaleY;
            float top = 0.0f;

            /* Use glm::ortho which handles the Y-flip correctly for ImGui */
            /* Note: glm::ortho(left, right, bottom, top) where bottom > top flips Y */
            m_ProjectionMatrix = glm::ortho(left, right, bottom, top, -1.0f, 1.0f);

            /* 更新UBO数据 */
            if (m_UIUniformBuffer)
            {
                ImGuiUniformData data;
                data.ProjectionMatrix = m_ProjectionMatrix;
                m_UIUniformBuffer->SetData(&data, sizeof(ImGuiUniformData));
            }
        }
    }

    void ImGuiRenderer::CreateFontTexture()
    {
        /* 获取字体图集数据 */
        ImGuiIO& io = ImGui::GetIO();
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        /* 创建纹理描述 */
        TextureDesc desc{};
        desc.Width = width;
        desc.Height = height;
        desc.Format = TextureFormat::RGBA8;
		desc.Usage = TextureUsage::Sampleable;

        /* 创建纹理 */
        m_FontTexture = DeviceTexture::Create("ImGuiFontTexture", desc);

        /* 填充纹理数据 */
        PixelDesc pixel_desc{};
        pixel_desc.Format = PixelFormat::RGBA;
		pixel_desc.Type = PixelType::UnsignedByte;
        m_FontTexture->SetData(pixels, pixel_desc);

        /* 设置ImGui字体纹理ID */
        io.Fonts->TexID = reinterpret_cast<ImTextureID>(m_FontTexture.get());

        CORE_LOG_INFO("ImGui font texture created: {}x{}", width, height);
    }

    void ImGuiRenderer::CreateUIShader()
    {
        m_UIShader = DeviceShader::Create(ABSOLUTE_PATH("Shaders/ImGuiUI.glsl"));
        if (m_UIShader && m_VertexArray)
        {
            /* 把顶点布局交给着色器：OpenGL 在 VAO 中记录属性位置，Metal 需要据此
             * 生成顶点描述符与管线状态。由后端在内部消化，调用方无需区分平台。 */
            m_UIShader->BindVertexArray(m_VertexArray);
        }

        CORE_LOG_INFO("ImGui UI shader created from embedded resource");
    }

    void ImGuiRenderer::CreateDepthShader()
    {
        /* 深度变体：同一顶点布局，片元按 depth2d<float>（Metal）/ 标准深度采样
         * （OpenGL）读取深度并输出灰度，供 Frame Graph 页显示深度附件预览 */
        m_UIDepthShader = DeviceShader::Create(ABSOLUTE_PATH("Shaders/ImGuiUIDepth.glsl"));
        if (m_UIDepthShader && m_VertexArray)
        {
            m_UIDepthShader->BindVertexArray(m_VertexArray);
        }

        CORE_LOG_INFO("ImGui depth preview shader created");
    }

    void ImGuiRenderer::EnsureBuffersCapacity(int vertex_count, int index_count)
    {
        /* 顶点缓冲区：几何增长扩容，避免UI抖动导致频繁重建GPU buffer */
        if (!m_VertexBuffer || m_VertexBufferSize < vertex_count)
        {
            int new_size = m_VertexBufferSize > 0 ? m_VertexBufferSize : 1000;
            while (new_size < vertex_count)
                new_size = new_size + new_size / 2; /* 1.5x growth */
            m_VertexBufferSize = new_size;
            m_VertexBuffer = DeviceVertexBuffer::Create("ImGui_VertexBuffer", m_VertexBufferSize * sizeof(ImDrawVert));

            /* 设置顶点布局 */
            VertexBufferLayout layout;
            layout.EmplaceElement(BufferElement{"Position", BufferDataType::Float2});
            layout.EmplaceElement(BufferElement{"TexCoord", BufferDataType::Float2});
            layout.EmplaceElement(BufferElement{"Color", BufferDataType::UByte4, true});  // normalized
            m_VertexBuffer->SetLayout(layout);

            /* 重新创建顶点数组并添加顶点缓冲区 */
            m_VertexArray = DeviceVertexArray::Create("ImGui_VertexArray");
            m_VertexArray->AddVertexBuffer(m_VertexBuffer);
            /* 若已有IndexBuffer，重新挂回VAO，保持索引绑定一致 */
            if (m_IndexBuffer)
                m_VertexArray->SetIndexBuffer(m_IndexBuffer);

            /* 顶点数组被重建，重新把布局同步给着色器（见 CreateUIShader） */
            if (m_UIShader)
                m_UIShader->BindVertexArray(m_VertexArray);
        }

        /* 索引缓冲区：几何增长扩容，避免频繁重建 */
        if (!m_IndexBuffer || m_IndexBufferSize < index_count)
        {
            int new_size = m_IndexBufferSize > 0 ? m_IndexBufferSize : 2000;
            while (new_size < index_count)
                new_size = new_size + new_size / 2; /* 1.5x growth */
            m_IndexBufferSize = new_size;
            /* ImGui 缺省 ImDrawIdx 为 unsigned short，对应 UInt16 */
            const IndexType index_type = (sizeof(ImDrawIdx) == 2) ? IndexType::UInt16 : IndexType::UInt32;
            m_IndexBuffer = IndexBuffer::Create("ImGui_IndexBuffer", static_cast<uint32_t>(m_IndexBufferSize), index_type);
            if (m_VertexArray)
                m_VertexArray->SetIndexBuffer(m_IndexBuffer);
        }
    }

    void ImGuiRenderer::RenderDrawList(const ImDrawList* draw_list, const ImDrawData* draw_data, int vertex_offset, int& index_offset)
    {
        /* 遍历所有命令 */
        for (int cmd_i = 0; cmd_i < draw_list->CmdBuffer.Size; cmd_i++)
        {
            const ImDrawCmd* pcmd = &draw_list->CmdBuffer[cmd_i];

            /* 设置裁剪区域 */
            if (pcmd->UserCallback)
            {
                /* 用户回调 */
                pcmd->UserCallback(draw_list, pcmd);
            }
            else
            {
                /* 设置裁剪区域 - 使用Scissor而不是Viewport */
                ImVec2 clip_off = draw_data->DisplayPos;
                ImVec2 clip_scale = draw_data->FramebufferScale;

                ImVec2 clip_min(
                    (pcmd->ClipRect.x - clip_off.x) * clip_scale.x,
                    (pcmd->ClipRect.y - clip_off.y) * clip_scale.y
                );
                ImVec2 clip_max(
                    (pcmd->ClipRect.z - clip_off.x) * clip_scale.x,
                    (pcmd->ClipRect.w - clip_off.y) * clip_scale.y
                );

                /* 边界检查 */
                if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y)
                    continue;

                /* 应用裁剪区域 - Metal坐标系Y轴向上，需要转换 */
                uint32_t scissor_x = static_cast<uint32_t>(clip_min.x);
                uint32_t scissor_y = static_cast<uint32_t>(m_DisplayHeight - clip_max.y);
                uint32_t scissor_width = static_cast<uint32_t>(clip_max.x - clip_min.x);
                uint32_t scissor_height = static_cast<uint32_t>(clip_max.y - clip_min.y);
                
                Renderer::GetRenderAPI()->SetScissor(scissor_x, scissor_y, scissor_width, scissor_height);

                /* 绑定纹理（如果不是默认字体纹理） */
                if (pcmd->TextureId)
                {
                    auto* texture = reinterpret_cast<DeviceTexture*>(pcmd->TextureId);
                    texture->Bind(0);
                }

                /* 绘制 - 索引已经在填充时调整为全局索引，直接使用index_offset */
                m_VertexArray->Bind();
                Renderer::GetRenderAPI()->DrawIndexed(
                    PrimitiveType::Triangles,
                    m_VertexArray,
                    pcmd->ElemCount,
                    index_offset  // 使用当前索引偏移
                );
            }

            index_offset += pcmd->ElemCount;
        }
    }
}
