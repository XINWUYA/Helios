#pragma once
#include <cstdint>
#include <vector>

#include <RmlUi/Core/RenderInterface.h>

#include "Helios/Common/Common.h"
#include "UIGlyphAtlas.h"

namespace Helios
{
	class DeviceTexture;

	namespace UI
	{
		/* RmlUi 的绘制录制层：元素树这一帧的几何按顺序摊进顶点 / 索引数组，
		 * 裁剪、纹理与变换随命令记下，回放交给 UIFrameRenderer。
		 * 框内元素有明确层叠语义，命令按录制顺序回放，不重排；同纹理同裁剪的相邻命令并成一条。
		 * 所有字体图集拼进一张纹理、纯色几何取它的白点，背景 / 边框 / 文字才能并在一起。
		 * 变换在摊平时就乘进位置（2D 仿射），回放不再逐笔切矩阵。 */
		class UIRenderInterface final : public Rml::RenderInterface
		{
		public:
			/* 一条回放命令：索引区间 + 纹理 + 录制时的裁剪框（RmlUi 坐标，左上原点） */
			struct DrawCommand
			{
				uint32_t FirstIndex{ 0 };
				uint32_t IndexCount{ 0 };
				uint32_t VertexCount{ 0 };
				DeviceTexture* Texture{ nullptr };
				bool ScissorEnabled{ false };
				Rml::Rectanglei Scissor{};
			};

			/* 一帧的录制结果：回放只读这一份，录制期间复用同一组数组 */
			struct Frame
			{
				std::vector<Rml::Vertex> Vertices{};
				std::vector<uint32_t> Indices{};
				std::vector<DrawCommand> Commands{};
			};

			UIRenderInterface();
			~UIRenderInterface() override;
			UIRenderInterface(const UIRenderInterface&) = delete;
			UIRenderInterface& operator=(const UIRenderInterface&) = delete;

			/* 开始一帧的录制：清掉上一帧的几何 */
			void BeginFrame();
			/* 进程退出前放掉编译好的几何与纹理 */
			void Shutdown();

			[[nodiscard]] const Frame& GetFrame() const { return m_Frame; }
			/* 生成出来的纹理（字体图集）当前占的字节数，进渲染统计 */
			[[nodiscard]] uint64_t GetGeneratedTextureBytes() const
			{
				return m_GeneratedTextureBytes + m_GlyphAtlas.GetBytes();
			}

			/* ---- Rml::RenderInterface ---- */
			Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
				Rml::Span<const int> indices) override;
			void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
				Rml::TextureHandle texture) override;
			void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

			Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
			Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source,
				Rml::Vector2i source_dimensions) override;
			void ReleaseTexture(Rml::TextureHandle texture) override;

			void EnableScissorRegion(bool enable) override;
			void SetScissorRegion(Rml::Rectanglei region) override;

			void SetTransform(const Rml::Matrix4f* transform) override;

		private:
			/* 一份编译好的几何：RmlUi 保证这块内存在 ReleaseGeometry 之前不变 */
			struct Geometry
			{
				std::vector<Rml::Vertex> Vertices{};
				std::vector<int> Indices{};
			};

			/* 纹理句柄 ↔ 纹理实例。句柄是下标 + 1，不能拿指针当句柄：
			 * 拼进字形图集的几份图集共用一张纹理，指针会撞 */
			struct TextureEntry
			{
				SharedPtr<DeviceTexture> Texture{};
				bool Owned{ false };
				bool Valid{ false };
				uint64_t Bytes{ 0 };
				/* 拼进图集的块：UV 要按它换算；InAtlas 为假时 Region 无意义 */
				bool InAtlas{ false };
				UIGlyphAtlas::Region Region{};
			};

			[[nodiscard]] Geometry* FindGeometry(Rml::CompiledGeometryHandle geometry) const;
			[[nodiscard]] const TextureEntry* FindTexture(Rml::TextureHandle texture) const;
			/* 登记一份纹理，返回句柄；释放过的槽位优先复用 */
			[[nodiscard]] Rml::TextureHandle RegisterTexture(TextureEntry entry);
			void ClearGeometries();

			std::vector<Geometry*> m_Geometries{};
			/* 释放过的槽位：句柄就是下标 + 1，复用能挡住长会话里的表增长 */
			std::vector<size_t> m_FreeSlots{};
			std::vector<TextureEntry> m_Textures{};
			std::vector<size_t> m_FreeTextureSlots{};
			uint64_t m_GeneratedTextureBytes{ 0 };
			/* 所有字体图集拼在这一张里：纯色几何借它的左上角白点 */
			UIGlyphAtlas m_GlyphAtlas{};

			Frame m_Frame{};

			bool m_ScissorEnabled{ false };
			Rml::Rectanglei m_Scissor{};
			/* 当前变换（RmlUi 在元素树里逐层下发），为空时是单位阵 */
			bool m_HasTransform{ false };
			float m_Transform[16]{};
		};
	}
}
