#include "Pch.h"
#include "UIRenderInterface.h"

#include "Helios/Core/AssetManager.h"
#include "Helios/Scene/SceneCommon.h"
#include "Helios/VirtualDevice/DeviceTexture.h"

namespace Helios
{
	namespace UI
	{
		UIRenderInterface::UIRenderInterface() = default;

		UIRenderInterface::~UIRenderInterface()
		{
			ClearGeometries();
		}

		void UIRenderInterface::BeginFrame()
		{
			m_Frame.Vertices.clear();
			m_Frame.Indices.clear();
			m_Frame.Commands.clear();
			m_ScissorEnabled = false;
			m_HasTransform = false;
		}

		void UIRenderInterface::Shutdown()
		{
			ClearGeometries();
			m_Textures.clear();
			m_FreeTextureSlots.clear();
			m_GeneratedTextureBytes = 0;
			m_GlyphAtlas = UIGlyphAtlas{};
			m_Frame.Vertices.clear();
			m_Frame.Indices.clear();
			m_Frame.Commands.clear();
		}

		Rml::CompiledGeometryHandle UIRenderInterface::CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
			Rml::Span<const int> indices)
		{
			auto* geometry = new Geometry;
			geometry->Vertices.assign(vertices.begin(), vertices.end());
			geometry->Indices.assign(indices.begin(), indices.end());

			/* 释放过的槽位先用：长会话里反复重载文档，句柄表不该一直长 */
			if (!m_FreeSlots.empty())
			{
				const size_t slot = m_FreeSlots.back();
				m_FreeSlots.pop_back();
				m_Geometries[slot] = geometry;
				return static_cast<Rml::CompiledGeometryHandle>(slot + 1);
			}

			m_Geometries.push_back(geometry);
			return static_cast<Rml::CompiledGeometryHandle>(m_Geometries.size());
		}

		void UIRenderInterface::RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
			Rml::TextureHandle texture)
		{
			const Geometry* compiled = FindGeometry(geometry);
			if (compiled == nullptr || compiled->Indices.empty())
				return;

			/* 纹理先定下来：拼进图集的块按块换算 UV，纯色几何取图集左上角的白点 */
			const TextureEntry* texture_entry = FindTexture(texture);
			const bool solid = texture == 0;
			const bool in_atlas = texture_entry != nullptr && texture_entry->InAtlas;
			DeviceTexture* device_texture = texture_entry != nullptr ? texture_entry->Texture.get() : nullptr;
			if (solid)
				device_texture = m_GlyphAtlas.GetTexture().get();

			const Rml::Vector2f white_uv = m_GlyphAtlas.GetWhiteUv();
			const float atlas_size = static_cast<float>(UIGlyphAtlas::kSize);

			/* 摊平：位置 = 当前变换 × (局部位置 + 平移)，回放时不再切矩阵 */
			const uint32_t first_vertex = static_cast<uint32_t>(m_Frame.Vertices.size());
			for (const Rml::Vertex& source : compiled->Vertices)
			{
				const float local_x = source.position.x + translation.x;
				const float local_y = source.position.y + translation.y;

				Rml::Vertex vertex = source;
				if (m_HasTransform)
				{
					vertex.position.x = m_Transform[0] * local_x + m_Transform[4] * local_y + m_Transform[12];
					vertex.position.y = m_Transform[1] * local_x + m_Transform[5] * local_y + m_Transform[13];
				}
				else
				{
					vertex.position.x = local_x;
					vertex.position.y = local_y;
				}

				if (solid)
				{
					vertex.tex_coord = white_uv;
				}
				else if (in_atlas)
				{
					const UIGlyphAtlas::Region& region = texture_entry->Region;
					vertex.tex_coord.x = (static_cast<float>(region.X) + vertex.tex_coord.x
						* static_cast<float>(region.Width)) / atlas_size;
					vertex.tex_coord.y = (static_cast<float>(region.Y) + vertex.tex_coord.y
						* static_cast<float>(region.Height)) / atlas_size;
				}
				m_Frame.Vertices.push_back(vertex);
			}

			const uint32_t first_index = static_cast<uint32_t>(m_Frame.Indices.size());
			for (const int source_index : compiled->Indices)
				m_Frame.Indices.push_back(first_vertex + static_cast<uint32_t>(source_index));

			/* 相邻命令同纹理同裁剪就并进上一条：索引本来就是顺序追加的，扩区间就够。
			 * 层叠顺序不变，只是少一次绘制调用 */
			if (!m_Frame.Commands.empty())
			{
				DrawCommand& last = m_Frame.Commands.back();
				if (last.Texture == device_texture && last.ScissorEnabled == m_ScissorEnabled
					&& (!m_ScissorEnabled || last.Scissor == m_Scissor))
				{
					last.IndexCount += static_cast<uint32_t>(compiled->Indices.size());
					last.VertexCount += static_cast<uint32_t>(compiled->Vertices.size());
					return;
				}
			}

			DrawCommand command;
			command.FirstIndex = first_index;
			command.IndexCount = static_cast<uint32_t>(compiled->Indices.size());
			command.VertexCount = static_cast<uint32_t>(compiled->Vertices.size());
			command.Texture = device_texture;
			command.ScissorEnabled = m_ScissorEnabled;
			command.Scissor = m_Scissor;
			m_Frame.Commands.push_back(command);
		}

		void UIRenderInterface::ReleaseGeometry(Rml::CompiledGeometryHandle geometry)
		{
			const size_t index = static_cast<size_t>(geometry);
			if (index == 0 || index > m_Geometries.size())
				return;

			delete m_Geometries[index - 1];
			m_Geometries[index - 1] = nullptr;
			m_FreeSlots.push_back(index - 1);
		}

		Rml::TextureHandle UIRenderInterface::LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source)
		{
			const std::string path = ABSOLUTE_PATH(source.c_str());
			SharedPtr<DeviceTexture> texture = AssetManager::Instance().GetOrLoad<DeviceTexture>(path);
			if (!texture || !texture->IsLoaded())
			{
				CORE_LOG_WARN("UI texture not found: {}", source.c_str());
				return 0;
			}

			texture_dimensions = Rml::Vector2i(static_cast<int>(texture->GetWidth()),
				static_cast<int>(texture->GetHeight()));

			TextureEntry entry;
			entry.Texture = texture;
			entry.Owned = false;
			entry.Valid = true;
			return RegisterTexture(std::move(entry));
		}

		Rml::TextureHandle UIRenderInterface::GenerateTexture(Rml::Span<const Rml::byte> source,
			Rml::Vector2i source_dimensions)
		{
			const uint32_t width = static_cast<uint32_t>(std::max(source_dimensions.x, 0));
			const uint32_t height = static_cast<uint32_t>(std::max(source_dimensions.y, 0));
			if (width == 0 || height == 0)
				return 0;

			/* 字体图集拼进公共图集：背景、边框与文字共用一张纹理，相邻命令才能合并 */
			UIGlyphAtlas::Region region;
			if (m_GlyphAtlas.Add(source, width, height, region))
			{
				TextureEntry entry;
				entry.Texture = m_GlyphAtlas.GetTexture();
				entry.InAtlas = true;
				entry.Region = region;
				entry.Valid = true;
				return RegisterTexture(std::move(entry));
			}

			/* 图集放不下（超大图或拼满了）：退回独立纹理，只是少一次合并机会 */
			TextureDesc desc{};
			desc.Width = width;
			desc.Height = height;
			desc.Format = TextureFormat::RGBA8;
			desc.Usage = TextureUsage::Sampleable;

			SharedPtr<DeviceTexture> texture = DeviceTexture::Create("UIGeneratedTexture", desc);
			if (!texture)
				return 0;

			PixelDesc pixel_desc{};
			pixel_desc.Format = PixelFormat::RGBA;
			pixel_desc.Type = PixelType::UnsignedByte;
			/* RmlUi 给的是预乘 RGBA，原样上传即可 */
			texture->SetData(const_cast<Rml::byte*>(source.data()), pixel_desc);

			TextureEntry entry;
			entry.Texture = texture;
			entry.Owned = true;
			entry.Bytes = static_cast<uint64_t>(width) * height * 4u;
			m_GeneratedTextureBytes += entry.Bytes;
			entry.Valid = true;
			return RegisterTexture(std::move(entry));
		}

		void UIRenderInterface::ReleaseTexture(Rml::TextureHandle texture)
		{
			const size_t index = static_cast<size_t>(texture);
			if (index == 0 || index > m_Textures.size())
				return;

			TextureEntry& entry = m_Textures[index - 1];
			if (!entry.Valid)
				return;

			if (entry.InAtlas)
				m_GlyphAtlas.Remove(entry.Region);
			if (entry.Owned)
				m_GeneratedTextureBytes -= entry.Bytes;

			entry = TextureEntry{};
			m_FreeTextureSlots.push_back(index - 1);
		}

		void UIRenderInterface::EnableScissorRegion(bool enable)
		{
			m_ScissorEnabled = enable;
		}

		void UIRenderInterface::SetScissorRegion(Rml::Rectanglei region)
		{
			m_Scissor = region;
		}

		void UIRenderInterface::SetTransform(const Rml::Matrix4f* transform)
		{
			if (transform == nullptr)
			{
				m_HasTransform = false;
				return;
			}

			m_HasTransform = true;
			for (int i = 0; i < 16; ++i)
				m_Transform[i] = transform->data()[i];
		}

		UIRenderInterface::Geometry* UIRenderInterface::FindGeometry(Rml::CompiledGeometryHandle geometry) const
		{
			const size_t index = static_cast<size_t>(geometry);
			return (index == 0 || index > m_Geometries.size()) ? nullptr : m_Geometries[index - 1];
		}

		const UIRenderInterface::TextureEntry* UIRenderInterface::FindTexture(Rml::TextureHandle texture) const
		{
			const size_t index = static_cast<size_t>(texture);
			if (index == 0 || index > m_Textures.size())
				return nullptr;

			const TextureEntry& entry = m_Textures[index - 1];
			return entry.Valid ? &entry : nullptr;
		}

		Rml::TextureHandle UIRenderInterface::RegisterTexture(TextureEntry entry)
		{
			if (!m_FreeTextureSlots.empty())
			{
				const size_t slot = m_FreeTextureSlots.back();
				m_FreeTextureSlots.pop_back();
				m_Textures[slot] = std::move(entry);
				return static_cast<Rml::TextureHandle>(slot + 1);
			}

			m_Textures.push_back(std::move(entry));
			return static_cast<Rml::TextureHandle>(m_Textures.size());
		}

		void UIRenderInterface::ClearGeometries()
		{
			for (Geometry* geometry : m_Geometries)
				delete geometry;
			m_Geometries.clear();
			m_FreeSlots.clear();
		}
	}
}
