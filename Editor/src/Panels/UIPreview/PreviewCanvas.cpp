#include "Pch.h"
#include "PreviewCanvas.h"

#include <RmlUi/Core.h>

#include "Helios/UI/UICanvasHost.h"

#include "Helios/Renderer/RenderAPI.h"
#include "Helios/Renderer/RenderCommon.h"
#include "Helios/Renderer/Renderer.h"
#include "Helios/VirtualDevice/DeviceFrameBuffer.h"
#include "Helios/VirtualDevice/DeviceTexture.h"

namespace Helios
{
	namespace UIEditor
	{
		namespace
		{
			/* 预览底：跟面板内容区同色，UI 没铺满的地方不会闪纯黑 */
			constexpr glm::vec4 kBackground{ 0.055f, 0.062f, 0.078f, 1.0f };

			const std::vector<std::string>& EmptyStrings()
			{
				static const std::vector<std::string> kEmpty;
				return kEmpty;
			}

			const std::string& EmptyString()
			{
				static const std::string kEmpty;
				return kEmpty;
			}
		}

		PreviewCanvas::PreviewCanvas() = default;

		PreviewCanvas::~PreviewCanvas()
		{
			Close();
		}

		bool PreviewCanvas::Open(const std::string& document_path)
		{
			Close();
			m_DocumentPath = document_path;
			m_Error.clear();

			if (m_Host == nullptr)
				m_Host = CreateUniquePtr<UI::UICanvasHost>();

			UI::UICanvasHost::Options options;
			options.ContextPrefix = "ui_preview";
			/* 建 Context 时给个能用的尺寸：1x1 会让文档在 Show 那一刻定成 0 尺寸 */
			options.Width = m_Width > 0 ? static_cast<int>(m_Width) : 1280;
			options.Height = m_Height > 0 ? static_cast<int>(m_Height) : 720;
			options.DensityRatio = m_DensityRatio;
			/* 预览连带盯同目录的样式表：<link> 由 RmlUi 在首次布局时才打开 */
			options.WatchSiblingStyles = true;

			if (!m_Host->Open(document_path, options))
			{
				m_Error = "加载失败：" + document_path;
				return false;
			}

			m_Selection.SetDensityRatio(m_DensityRatio);
			/* 打开就把文档交给选中层：元素树与撤销都按它找元素，
			 * 少了这一步要等用户先点一下画布才有内容 */
			m_Selection.SetDocument(m_Host->GetDocument());
			m_ElementCount = m_Host->CountElements();
			return true;
		}

		void PreviewCanvas::Close()
		{
			if (m_Host != nullptr)
				m_Host->Close();

			/* 文档重开（含热重载）后元素上的变换已经没了，状态表必须跟着清：
			 * 留着旧 translate 会让选框飘到元素以前待过的地方 */
			m_Selection.Reset();
			m_ElementCount = 0;
		}

		void PreviewCanvas::SetDensityRatio(float ratio)
		{
			m_DensityRatio = ratio > 0.0f ? ratio : 1.0f;
			m_Selection.SetDensityRatio(m_DensityRatio);
			if (m_Host != nullptr)
				m_Host->SetDensityRatio(m_DensityRatio);
		}

		bool PreviewCanvas::CreateTarget(uint32_t width, uint32_t height)
		{
			TextureDesc desc{};
			desc.Width = width;
			desc.Height = height;
			desc.Format = TextureFormat::RGBA8;
			desc.Usage = TextureUsage::ColorAttachment | TextureUsage::Sampleable | TextureUsage::Uploadable;
			m_Color = DeviceTexture::Create("UIPreviewTarget", desc);
			if (m_Color == nullptr)
			{
				m_Error = "无法创建预览纹理";
				return false;
			}

			FrameBufferDesc frame_buffer_desc{};
			frame_buffer_desc.ViewportRegion = { 0, 0, width, height };
			frame_buffer_desc.Usage = RenderBufferUsage::Color0;
			RenderBufferInfo color_info{};
			color_info.RenderTarget = m_Color;
			frame_buffer_desc.ColorRenderBuffers.push_back(color_info);
			/* 每帧按面板底色刷干净：不给清除值的话目标里是上一次的残留 */
			frame_buffer_desc.ColorClearValues.push_back(kBackground);

			m_Target = DeviceFrameBuffer::Create("UIPreviewTarget", frame_buffer_desc);
			if (m_Target == nullptr)
			{
				m_Error = "无法创建预览帧缓冲";
				return false;
			}

			m_Width = width;
			m_Height = height;
			return true;
		}

		bool PreviewCanvas::Render(uint32_t width, uint32_t height)
		{
			if (m_Host == nullptr || !m_Host->IsOpen() || width == 0 || height == 0)
				return false;

			const SharedPtr<RenderAPI> render_api = Renderer::GetRenderAPI();
			if (!render_api)
				return false;

			/* 尺寸变了：目标重画；Context 尺寸由宿主的 Update / Render 一起带过去 */
			if (width != m_Width || height != m_Height)
			{
				if (!CreateTarget(width, height))
					return false;
			}

			m_Host->Update(static_cast<int>(width), static_cast<int>(height));

			m_Target->Bind();
			render_api->SetViewport(0, 0, width, height);
			render_api->SetScissor(0, 0, width, height);
			m_Host->Render(static_cast<int>(width), static_cast<int>(height));
			m_Target->Unbind();
			return true;
		}

		PreviewCanvas::Change PreviewCanvas::ReloadIfChanged()
		{
			if (m_Host == nullptr || !m_Host->IsOpen())
				return Change::None;

			switch (m_Host->ReloadIfChanged())
			{
			case UI::UICanvasHost::Change::Document:
			{
				/* 结构变了：整份重开（尺寸保留） */
				const std::string path = m_DocumentPath;
				if (Open(path))
					return Change::Document;
				m_Error = "重载失败：" + path;
				return Change::None;
			}
			case UI::UICanvasHost::Change::Style:
				return Change::Style;
			default:
				return Change::None;
			}
		}

		bool PreviewCanvas::ProcessMouseMove(float x, float y)
		{
			return m_Host != nullptr && m_Host->ProcessMouseMove(x, y);
		}

		bool PreviewCanvas::ProcessMouseButtonDown(int button)
		{
			return m_Host != nullptr && m_Host->ProcessMouseButtonDown(button);
		}

		bool PreviewCanvas::ProcessMouseButtonUp(int button)
		{
			return m_Host != nullptr && m_Host->ProcessMouseButtonUp(button);
		}

		bool PreviewCanvas::ProcessMouseWheel(float delta)
		{
			return m_Host != nullptr && m_Host->ProcessMouseWheel(delta);
		}

		void PreviewCanvas::ProcessMouseLeave()
		{
			if (m_Host != nullptr)
				m_Host->ProcessMouseLeave();
		}

		bool PreviewCanvas::ProcessKeyDown(KeyCode key)
		{
			return m_Host != nullptr && m_Host->ProcessKeyDown(key);
		}

		bool PreviewCanvas::ProcessKeyUp(KeyCode key)
		{
			return m_Host != nullptr && m_Host->ProcessKeyUp(key);
		}

		bool PreviewCanvas::ProcessTextInput(uint32_t codepoint)
		{
			return m_Host != nullptr && m_Host->ProcessTextInput(codepoint);
		}

		bool PreviewCanvas::HitTest(float x, float y, ElementHit& out) const
		{
			if (m_Host == nullptr || !m_Host->IsOpen())
				return false;
			return m_Selection.HitTest(*m_Host->GetContext(), x, y, out);
		}

		bool PreviewCanvas::SelectAt(float x, float y)
		{
			if (m_Host == nullptr || !m_Host->IsOpen())
				return false;
			return m_Selection.SelectAt(*m_Host->GetContext(), m_Host->GetDocument(), x, y);
		}

		void PreviewCanvas::ClearSelection()
		{
			m_Selection.Clear();
		}

		bool PreviewCanvas::HasSelection() const
		{
			return m_Selection.HasSelection();
		}

		std::string PreviewCanvas::GetSelectionId() const
		{
			return m_Selection.GetId();
		}

		bool PreviewCanvas::GetSelectionBox(float& x, float& y, float& width, float& height) const
		{
			return m_Selection.GetBox(x, y, width, height);
		}

		bool PreviewCanvas::GetSelectionPosition(float& left, float& top) const
		{
			return m_Selection.GetPosition(left, top);
		}

		void PreviewCanvas::SetSelectionPosition(float left, float top)
		{
			m_Selection.SetPosition(left, top);
		}

		void PreviewCanvas::GetSelectionRenderQuad(float (&corners)[4][2]) const
		{
			m_Selection.GetRenderQuad(corners);
		}

		void PreviewCanvas::BeginSelectionDrag()
		{
			m_Selection.BeginDrag();
		}

		bool PreviewCanvas::DragSelection(float dx, float dy)
		{
			return m_Selection.Drag(dx, dy);
		}

		void PreviewCanvas::EndSelectionDrag()
		{
			m_Selection.EndDrag();
		}

		bool PreviewCanvas::GetSelectionState(PreviewSelection::ElementState& out) const
		{
			return m_Selection.GetSelectionState(out);
		}

		bool PreviewCanvas::ApplyElementState(const PreviewSelection::ElementKey& key,
			const PreviewSelection::ElementState& state)
		{
			return m_Selection.ApplyState(key, state);
		}

		Rml::Element* PreviewCanvas::GetSelectionElement() const
		{
			return m_Selection.GetElement();
		}

		void PreviewCanvas::CollectSelectionProperties(std::vector<ElementPropertyValue>& out) const
		{
			CollectElementProperties(m_Selection.GetElement(), out);
		}

		bool PreviewCanvas::ApplyElementProperty(const PreviewSelection::ElementKey& key,
			const ElementPropertyDesc& desc, const std::string& value)
		{
			return WriteElementProperty(m_Selection.FindElement(key), desc, value);
		}

		bool PreviewCanvas::ReadElementPropertyValue(const PreviewSelection::ElementKey& key,
			const ElementPropertyDesc& desc, std::string& out) const
		{
			return ReadElementProperty(m_Selection.FindElement(key), desc, out);
		}

		void PreviewCanvas::CollectElementStyles(const PreviewSelection::ElementKey& key,
			std::vector<std::pair<std::string, std::string>>& out) const
		{
			out.clear();

			Rml::Element* element = m_Selection.FindElement(key);
			if (element == nullptr)
				return;

			for (const auto& entry : element->GetLocalStyleProperties())
			{
				const Rml::String& name = Rml::StyleSheetSpecification::GetPropertyName(entry.first);
				out.emplace_back(name.c_str(), entry.second.ToString());
			}
		}

		bool PreviewCanvas::SetElementId(const PreviewSelection::ElementKey& key, const std::string& id)
		{
			Rml::Element* element = m_Selection.FindElement(key);
			if (element == nullptr || id.empty())
				return false;

			element->SetAttribute("id", Rml::String(id));
			return true;
		}

		bool PreviewCanvas::HasElementId(const std::string& id) const
		{
			return m_Selection.FindElementById(id) != nullptr;
		}

		std::string PreviewCanvas::GetElementTag(const PreviewSelection::ElementKey& key) const
		{
			Rml::Element* element = m_Selection.FindElement(key);
			return element != nullptr ? std::string(element->GetTagName().c_str()) : std::string();
		}

		void PreviewCanvas::CollectElements(std::vector<PreviewSelection::ElementNode>& out) const
		{
			m_Selection.CollectElements(out);
		}

		bool PreviewCanvas::SelectIndex(std::size_t index)
		{
			return m_Selection.SelectIndex(index);
		}

		int PreviewCanvas::GetSelectionIndex() const
		{
			return m_Selection.GetSelectionIndex();
		}

		bool PreviewCanvas::SelectById(const std::string& id)
		{
			return m_Selection.SelectById(id);
		}

		bool PreviewCanvas::IsSelectionTransformOpaque() const
		{
			return m_Selection.IsTransformOpaque();
		}

		bool PreviewCanvas::GetSelectionAnchorPoint(float& x, float& y) const
		{
			return m_Selection.GetAnchorPoint(x, y);
		}

		void PreviewCanvas::SetSelectionTransform(float rotation_degrees, float scale)
		{
			m_Selection.SetTransform(rotation_degrees, scale);
		}

		void PreviewCanvas::SetSelectionTransformState(float rotation_degrees, float scale)
		{
			m_Selection.SetTransformState(rotation_degrees, scale);
		}

		void PreviewCanvas::SetSelectionAnchor(float anchor_x, float anchor_y)
		{
			m_Selection.SetAnchor(anchor_x, anchor_y);
		}

		void PreviewCanvas::GetSelectionTranslate(float& x, float& y) const
		{
			m_Selection.GetTranslate(x, y);
		}

		void PreviewCanvas::SetSelectionTranslate(float x, float y)
		{
			m_Selection.SetTranslate(x, y);
		}

		void PreviewCanvas::GetSelectionAncestorTranslate(float& x, float& y) const
		{
			m_Selection.GetAncestorTranslate(x, y);
		}

		float PreviewCanvas::GetSelectionRotation() const
		{
			return m_Selection.GetRotation();
		}

		float PreviewCanvas::GetSelectionScale() const
		{
			return m_Selection.GetScale();
		}

		float PreviewCanvas::GetAnchorX() const
		{
			return m_Selection.GetAnchorX();
		}

		float PreviewCanvas::GetAnchorY() const
		{
			return m_Selection.GetAnchorY();
		}

		bool PreviewCanvas::IsSelectionStatic() const
		{
			return m_Selection.IsStatic();
		}

		bool PreviewCanvas::HasSelectionTransform() const
		{
			return m_Selection.HasTransform();
		}

		bool PreviewCanvas::IsOpen() const
		{
			return m_Host != nullptr && m_Host->IsOpen();
		}

		const std::vector<std::string>& PreviewCanvas::GetMissingFiles() const
		{
			return m_Host != nullptr ? m_Host->GetMissingFiles() : EmptyStrings();
		}

		const std::string& PreviewCanvas::GetLastMessage() const
		{
			return m_Host != nullptr ? m_Host->GetLastMessage() : EmptyString();
		}
	}
}
