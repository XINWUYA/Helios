#include "Pch.h"
#include "UIFrameRenderer.h"

#include "UIRenderInterface.h"

#include "Helios/Core/AssetManager.h"
#include "Helios/Profiler/RenderStatistics.h"
#include "Helios/Renderer/RenderAPI.h"
#include "Helios/Renderer/Renderer.h"
#include "Helios/Scene/SceneCommon.h"
#include "Helios/VirtualDevice/DeviceShader.h"
#include "Helios/VirtualDevice/DeviceTexture.h"
#include "Helios/VirtualDevice/DeviceVertexArray.h"

namespace Helios
{
	namespace UI
	{
		namespace
		{
			/* 顶点布局必须与 Rml::Vertex 的内存顺序一致：位置、预乘颜色、纹理坐标。
			 * 名字只用来在着色器里对位，偏移是按下标累加的，顺序错了就是整片乱码 */
			VertexBufferLayout MakeVertexLayout()
			{
				static_assert(sizeof(Rml::Vertex) == 20, "Rml::Vertex layout changed");
				static_assert(offsetof(Rml::Vertex, colour) == 8, "Rml::Vertex colour offset changed");
				static_assert(offsetof(Rml::Vertex, tex_coord) == 12, "Rml::Vertex texcoord offset changed");

				VertexBufferLayout layout;
				layout.EmplaceElement(BufferElement{ "a_Position", BufferDataType::Float2 });
				layout.EmplaceElement(BufferElement{ "a_Color", BufferDataType::UByte4, true });
				layout.EmplaceElement(BufferElement{ "a_TexCoord", BufferDataType::Float2 });
				return layout;
			}
		}

		void UIFrameRenderer::Init()
		{
			/* 不走材质：直接按路径取着色器，改完源码热重载能覆盖到 */
			m_Shader = AssetManager::Instance().GetOrLoad<DeviceShader>(
				ABSOLUTE_PATH("Shaders/UI/UIShader.glsl"));
			if (!m_Shader)
				CORE_LOG_ERROR("UI shader is missing: Shaders/UI/UIShader.glsl");

			TextureDesc desc{};
			desc.Width = 1;
			desc.Height = 1;
			desc.Format = TextureFormat::RGBA8;
			desc.Usage = TextureUsage::Sampleable;
			m_WhiteTexture = DeviceTexture::Create("UIWhiteTexture", desc);
			const uint8_t white[4] = { 255, 255, 255, 255 };
			PixelDesc pixel_desc{};
			pixel_desc.Format = PixelFormat::RGBA;
			pixel_desc.Type = PixelType::UnsignedByte;
			m_WhiteTexture->SetData(const_cast<uint8_t*>(white), pixel_desc);
		}

		void UIFrameRenderer::Shutdown()
		{
			ReleaseBuffers();
			m_Shader.reset();
			m_WhiteTexture.reset();
		}

		void UIFrameRenderer::Draw(const UIRenderInterface& recorder, uint32_t target_width, uint32_t target_height)
		{
			const UIRenderInterface::Frame& frame = recorder.GetFrame();
			if (frame.Commands.empty() || !m_Shader || target_width == 0 || target_height == 0)
				return;

			const SharedPtr<RenderAPI> render_api = Renderer::GetRenderAPI();
			if (render_api == nullptr)
				return;

			EnsureBufferCapacity(frame.Vertices.size(), frame.Indices.size());

			const int slot = m_FrameSlot;
			m_FrameSlot = (m_FrameSlot + 1) % kFramesInFlight;
			m_VertexBuffers[slot]->SetData(frame.Vertices.data(),
				static_cast<uint32_t>(frame.Vertices.size() * sizeof(Rml::Vertex)));
			m_IndexBuffers[slot]->SetData(frame.Indices.data(),
				static_cast<uint32_t>(frame.Indices.size() * sizeof(uint32_t)));

			/* 预乘 alpha：颜色与图集都按预乘存，混合成 One / OneMinusSrcAlpha。
			 * 与 ImGui 的直通 alpha 不是一套状态，不能共用 */
			RenderRasterState raster_state;
			raster_state.CullMode = CullMode::Cull_None;
			raster_state.EnableBlend = true;
			raster_state.BlendEquationRGB = BlendEquation::Add;
			raster_state.BlendEquationA = BlendEquation::Add;
			raster_state.BlendFuncSrcRGB = BlendFunc::One;
			raster_state.BlendFuncSrcA = BlendFunc::One;
			raster_state.BlendFuncDstRGB = BlendFunc::OneMinusSrcAlpha;
			raster_state.BlendFuncDstA = BlendFunc::OneMinusSrcAlpha;
			raster_state.EnableDepthTest = false;
			raster_state.EnableDepthWrite = false;
			raster_state.EnableColorWrite = true;
			render_api->ApplyRasterState(raster_state);
			render_api->SetViewport(0, 0, target_width, target_height);

			/* 屏幕空间正交：原点左上、y 向下，与 RmlUi 的坐标一致 */
			const glm::mat4 projection = glm::ortho(0.0f, static_cast<float>(target_width),
				static_cast<float>(target_height), 0.0f, -1.0f, 1.0f);

			m_Shader->Bind();
			const int sampler_slot = std::max(m_Shader->GetUniformBinding("u_Texture"), 0);
			m_Shader->SetInt("u_Texture", sampler_slot);
			m_Shader->SetMat4("u_Projection", projection);
			m_Shader->BindVertexArray(m_VertexArrays[slot]);
			m_VertexArrays[slot]->Bind();

			RenderStatistics& statistics = RenderStatistics::Instance();
			statistics.SetUIFontAtlasBytes(recorder.GetGeneratedTextureBytes());

			/* 相邻命令经常共用裁剪或纹理：状态没变就不重复设置 */
			uint32_t scissor_x = 0;
			uint32_t scissor_y = 0;
			uint32_t scissor_width = 0;
			uint32_t scissor_height = 0;
			bool has_scissor = false;
			DeviceTexture* bound_texture = nullptr;

			for (const UIRenderInterface::DrawCommand& command : frame.Commands)
			{
				statistics.AddUIDrawCall(command.VertexCount);
				/* 裁剪框按 OpenGL 左下角约定给出：RmlUi 是左上角，这里翻一次 Y */
				uint32_t next_x = 0;
				uint32_t next_y = 0;
				uint32_t next_width = target_width;
				uint32_t next_height = target_height;
				if (command.ScissorEnabled)
				{
					const Rml::Rectanglei& region = command.Scissor;
					const int top = std::max(region.Top(), 0);
					const int bottom = std::min(region.Bottom(), static_cast<int>(target_height));
					if (region.Width() <= 0 || bottom <= top)
						continue;

					next_x = static_cast<uint32_t>(std::max(region.Left(), 0));
					next_y = static_cast<uint32_t>(static_cast<int>(target_height) - bottom);
					next_width = static_cast<uint32_t>(region.Width());
					next_height = static_cast<uint32_t>(bottom - top);
				}

				if (!has_scissor || next_x != scissor_x || next_y != scissor_y
					|| next_width != scissor_width || next_height != scissor_height)
				{
					render_api->SetScissor(next_x, next_y, next_width, next_height);
					scissor_x = next_x;
					scissor_y = next_y;
					scissor_width = next_width;
					scissor_height = next_height;
					has_scissor = true;
				}

				DeviceTexture* texture = command.Texture != nullptr ? command.Texture : m_WhiteTexture.get();
				if (texture != nullptr && texture != bound_texture)
				{
					texture->Bind(static_cast<uint32_t>(sampler_slot));
					bound_texture = texture;
				}

				render_api->DrawIndexed(PrimitiveType::Triangles, m_VertexArrays[slot],
					command.IndexCount, command.FirstIndex);
			}

			m_VertexArrays[slot]->Unbind();
			m_Shader->Unbind();
			render_api->SetScissor(0, 0, target_width, target_height);
		}

		void UIFrameRenderer::EnsureBufferCapacity(size_t vertex_count, size_t index_count)
		{
			const bool vertex_grow = m_VertexCapacity < vertex_count;
			const bool index_grow = m_IndexCapacity < index_count;
			if (!vertex_grow && !index_grow && m_VertexArrays[0])
				return;

			if (vertex_grow)
			{
				size_t capacity = m_VertexCapacity > 0 ? m_VertexCapacity : 2048;
				while (capacity < vertex_count)
					capacity += capacity / 2;
				m_VertexCapacity = capacity;
			}
			if (index_grow)
			{
				size_t capacity = m_IndexCapacity > 0 ? m_IndexCapacity : 4096;
				while (capacity < index_count)
					capacity += capacity / 2;
				m_IndexCapacity = capacity;
			}

			for (int slot = 0; slot < kFramesInFlight; ++slot)
			{
				m_VertexBuffers[slot] = DeviceVertexBuffer::Create("UI_VertexBuffer",
					static_cast<uint32_t>(m_VertexCapacity) * sizeof(Rml::Vertex));
				m_VertexBuffers[slot]->SetLayout(MakeVertexLayout());
				m_IndexBuffers[slot] = IndexBuffer::Create("UI_IndexBuffer",
					static_cast<uint32_t>(m_IndexCapacity), IndexType::UInt32);
				m_VertexArrays[slot] = DeviceVertexArray::Create("UI_VertexArray");
				m_VertexArrays[slot]->AddVertexBuffer(m_VertexBuffers[slot]);
				m_VertexArrays[slot]->SetIndexBuffer(m_IndexBuffers[slot]);

				/* 顶点数组换了对象，把布局重新同步给着色器（Metal 按它生成顶点描述符） */
				if (m_Shader)
					m_Shader->BindVertexArray(m_VertexArrays[slot]);
			}
		}

		void UIFrameRenderer::ReleaseBuffers()
		{
			for (int slot = 0; slot < kFramesInFlight; ++slot)
			{
				m_VertexArrays[slot].reset();
				m_VertexBuffers[slot].reset();
				m_IndexBuffers[slot].reset();
			}
			m_VertexCapacity = 0;
			m_IndexCapacity = 0;
			m_FrameSlot = 0;
		}
	}
}
