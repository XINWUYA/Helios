#pragma once
#include <cstddef>
#include <cstdint>
#include <utility>
#include <string>
#include <vector>

#include "Helios/Common/Common.h"
#include "Helios/Core/KeyCodes.h"
#include "ElementProperties.h"
#include "PreviewSelection.h"

namespace Helios
{
	class DeviceFrameBuffer;
	class DeviceTexture;

	namespace UI
	{
		class UICanvasHost;
	}

	namespace UIEditor
	{
		/* 编辑器预览用的独立画布：自己一个 Context，画进自己的纹理，不参与场景合成。
		 * 与场景画布共用文档宿主（UICanvasHost）、渲染层与文件解析，只是没有实体也没有 Scene。
		 * 在编辑器模块里编译：选中与变换要直接调 RmlUi 的元素与属性接口。 */
		class PreviewCanvas final
		{
		public:
			PreviewCanvas();
			~PreviewCanvas();
			PreviewCanvas(const PreviewCanvas&) = delete;
			PreviewCanvas& operator=(const PreviewCanvas&) = delete;

			/* 打开一份文档（相对项目根）；成功返回 true，失败时 GetError 给出原因 */
			bool Open(const std::string& document_path);
			void Close();

			/* dp 与物理像素的比例：Retina 上传 2 才够清晰，元素视觉大小不变 */
			void SetDensityRatio(float ratio);

			/* 推一帧：尺寸（物理像素）变了重建目标、布局、画进目标纹理 */
			bool Render(uint32_t width, uint32_t height);

			/* 磁盘上改了什么：文档要整份重开，样式就地重排，撤销记录的处理也不同 */
			enum class Change
			{
				None,
				Document,
				Style,
			};
			Change ReloadIfChanged();

			/* ---- 指针（目标纹理像素坐标，原点左上角）：返回 UI 是否消费 ---- */
			bool ProcessMouseMove(float x, float y);
			bool ProcessMouseButtonDown(int button);
			bool ProcessMouseButtonUp(int button);
			bool ProcessMouseWheel(float delta);
			void ProcessMouseLeave();

			/* ---- 键盘与文本：编辑器把焦点在面板上时的按键与字符递进来 ---- */
			bool ProcessKeyDown(KeyCode key);
			bool ProcessKeyUp(KeyCode key);
			bool ProcessTextInput(uint32_t codepoint);

			/* 命中测试：坐标为目标纹理像素，返回是否命中元素 */
			bool HitTest(float x, float y, ElementHit& out) const;

			/* ---- 选中：点一下锁定一个元素，供面板显示与后续就地调整 ---- */
			bool SelectAt(float x, float y);
			void ClearSelection();
			[[nodiscard]] bool HasSelection() const;

			[[nodiscard]] std::string GetSelectionId() const;
			[[nodiscard]] bool GetSelectionBox(float& x, float& y, float& width, float& height) const;
			/* 选中元素的声明位置：dp 值（由盒子折回，父级在原点时与 left/top 一致） */
			[[nodiscard]] bool GetSelectionPosition(float& left, float& top) const;
			/* 只改 left/top（撤销时用绝对写回） */
			void SetSelectionPosition(float left, float top);
			/* 选中元素经「自身 + 祖先」变换后的四角（布局像素、绝对坐标） */
			void GetSelectionRenderQuad(float (&corners)[4][2]) const;

			/* ---- 就地拖动 ---- */
			void BeginSelectionDrag();
			bool DragSelection(float dx, float dy);
			void EndSelectionDrag();

			/* ---- 撤销用：按 key 读写整套状态，不依赖当前选中 ---- */
			bool GetSelectionState(PreviewSelection::ElementState& out) const;
			bool ApplyElementState(const PreviewSelection::ElementKey& key,
				const PreviewSelection::ElementState& state);

			/* ---- 属性面板：选中元素当前能改的属性 ---- */
			[[nodiscard]] Rml::Element* GetSelectionElement() const;
			void CollectSelectionProperties(std::vector<ElementPropertyValue>& out) const;
			/* 按 key 写一个属性，撤销与面板共用；元素不在返回 false */
			bool ApplyElementProperty(const PreviewSelection::ElementKey& key,
				const ElementPropertyDesc& desc, const std::string& value);

			/* ---- 保存用：按 key 读元素上的值（元素可能已经不是当前选中）---- */
			bool ReadElementPropertyValue(const PreviewSelection::ElementKey& key,
				const ElementPropertyDesc& desc, std::string& out) const;
			void CollectElementStyles(const PreviewSelection::ElementKey& key,
				std::vector<std::pair<std::string, std::string>>& out) const;
			/* 元素没有 id 时补一个：写回 .css 与撤销都要锚点 */
			bool SetElementId(const PreviewSelection::ElementKey& key, const std::string& id);
			[[nodiscard]] bool HasElementId(const std::string& id) const;
			[[nodiscard]] std::string GetElementTag(const PreviewSelection::ElementKey& key) const;

			/* ---- 元素树：结构快照与按行选中 ---- */
			void CollectElements(std::vector<PreviewSelection::ElementNode>& out) const;
			bool SelectIndex(std::size_t index);
			bool SelectById(const std::string& id);
			/* 选中元素自带的变换表达不了（matrix / skew / 3D） */
			[[nodiscard]] bool IsSelectionTransformOpaque() const;
			/* 实际的旋转锚点（纹理坐标） */
			bool GetSelectionAnchorPoint(float& x, float& y) const;
			[[nodiscard]] int GetSelectionIndex() const;

			/* ---- 旋转 / 缩放 ---- */
			void SetSelectionTransform(float rotation_degrees, float scale);
			/* 只更新记下的值，不写属性（面板自己改数值时用） */
			void SetSelectionTransformState(float rotation_degrees, float scale);
			/* 缩放 / 旋转的原点：0~1 的分数（0 = 左/上，0.5 = 中心，1 = 右/下） */
			void SetSelectionAnchor(float anchor_x, float anchor_y);
			/* 位移量（dp）：静态元素（flex 子项、普通流）动不了 left/top，改走 translate */
			void GetSelectionTranslate(float& x, float& y) const;
			void SetSelectionTranslate(float x, float y);
			/* 祖先链上的位移合计（dp） */
			void GetSelectionAncestorTranslate(float& x, float& y) const;

			[[nodiscard]] float GetSelectionRotation() const;
			[[nodiscard]] float GetSelectionScale() const;
			[[nodiscard]] float GetAnchorX() const;
			[[nodiscard]] float GetAnchorY() const;
			/* 元素是不是静态定位：静态元素写 left/top 无效，得写 transform: translate */
			[[nodiscard]] bool IsSelectionStatic() const;
			/* 变换属性是否真的挂上去了（调试与检查用） */
			[[nodiscard]] bool HasSelectionTransform() const;

			[[nodiscard]] bool IsOpen() const;
			[[nodiscard]] const std::string& GetDocumentPath() const { return m_DocumentPath; }
			[[nodiscard]] const std::string& GetError() const { return m_Error; }
			[[nodiscard]] const SharedPtr<DeviceTexture>& GetTexture() const { return m_Color; }
			/* 最近一帧画出来的尺寸（面板按它算显示区域） */
			[[nodiscard]] uint32_t GetWidth() const { return m_Width; }
			[[nodiscard]] uint32_t GetHeight() const { return m_Height; }
			/* 文档里的元素数（打开时数一次），状态行显示用 */
			[[nodiscard]] size_t GetElementCount() const { return m_ElementCount; }
			/* 加载期间打开失败的文件（href 写错这类问题） */
			[[nodiscard]] const std::vector<std::string>& GetMissingFiles() const;
			/* 最近一条 RmlUi 警告 / 错误 */
			[[nodiscard]] const std::string& GetLastMessage() const;

		private:
			bool CreateTarget(uint32_t width, uint32_t height);

			UniquePtr<UI::UICanvasHost> m_Host{};
			PreviewSelection m_Selection{};

			std::string m_DocumentPath{};
			std::string m_Error{};

			SharedPtr<DeviceTexture> m_Color{};
			SharedPtr<DeviceFrameBuffer> m_Target{};
			uint32_t m_Width{ 0 };
			uint32_t m_Height{ 0 };
			float m_DensityRatio{ 1.0f };
			size_t m_ElementCount{ 0 };
		};
	}
}
