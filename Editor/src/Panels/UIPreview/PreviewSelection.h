#pragma once
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Rml
{
	class Context;
	class Element;
	class ElementDocument;
}

namespace Helios
{
	namespace UIEditor
	{
		/* 光标下元素的检查结果：编辑器用来画高亮框与信息条 */
		struct ElementHit
		{
			std::string Id{};
			std::string ClassName{};
			float X{ 0.0f };
			float Y{ 0.0f };
			float Width{ 0.0f };
			float Height{ 0.0f };
			/* 盒子超出父级（布局写错的常见症状） */
			bool OverflowX{ false };
			bool OverflowY{ false };
			/* 经自身与祖先变换后的四角（布局像素，绝对坐标）：高亮框按它画才跟得上父级 */
			float Corners[4][2]{};
		};

		/* 预览里的选中与变换：改的是元素属性，不落盘也不碰渲染。
		 * 每个元素的状态按 id 记一份，画布重开时清空（元素上的变换已经没了）。 */
		class PreviewSelection final
		{
		public:
			/* 一个元素的整套变换状态：按 id 记一份，撤销时整份读回 */
			struct ElementState
			{
				float Left{ 0.0f };
				float Top{ 0.0f };
				float TranslateX{ 0.0f };
				float TranslateY{ 0.0f };
				float Rotate{ 0.0f };
				float Scale{ 1.0f };
				float AnchorX{ 0.5f };
				float AnchorY{ 0.5f };
			};

			/* 文档重开：元素与状态都作废 */
			void Reset();
			/* 绑上当前文档：树、按行选中、撤销都靠它找元素。
			 * 打开文档时就要绑，不能等用户先点一下画布 */
			void SetDocument(Rml::ElementDocument* document) { m_Document = document; }

			/* dp 与物理像素的比例：写回 left/top 与 transform 时按它换算 */
			void SetDensityRatio(float ratio);

			/* 点一下锁定一个元素；点到空白处（body / 文档本身）算取消选中 */
			bool SelectAt(Rml::Context& context, Rml::ElementDocument* document, float x, float y);
			void Clear();
			[[nodiscard]] bool HasSelection() const { return m_Element != nullptr; }
			[[nodiscard]] std::string GetId() const;
			[[nodiscard]] Rml::Element* GetElement() const { return m_Element; }

			/* 盒子（布局像素、绝对坐标）与声明位置（dp，相对包含块） */
			[[nodiscard]] bool GetBox(float& x, float& y, float& width, float& height) const;
			[[nodiscard]] bool GetPosition(float& left, float& top) const;
			void SetPosition(float left, float top);

			/* 就地拖动：dx/dy 是物理像素，按 dp 比例换算后写 left/top（inline 属性，不落盘） */
			void BeginDrag();
			bool Drag(float dx, float dy);
			void EndDrag();

			/* 旋转 / 缩放：写 transform 的 inline 属性（累积值由调用方维护） */
			void SetTransform(float rotation_degrees, float scale);
			void SetTransformState(float rotation_degrees, float scale);
			/* 位移量（dp）：静态元素（flex 子项、普通流）动不了 left/top，改走 translate */
			void SetTranslate(float x, float y);
			void GetTranslate(float& x, float& y) const;
			/* 缩放 / 旋转的原点：0~1 的分数（0 = 左/上，0.5 = 中心，1 = 右/下） */
			void SetAnchor(float x, float y);

			[[nodiscard]] float GetRotation() const { return m_Rotation; }
			[[nodiscard]] float GetScale() const { return m_Scale; }
			[[nodiscard]] float GetAnchorX() const { return m_AnchorX; }
			[[nodiscard]] float GetAnchorY() const { return m_AnchorY; }
			/* 静态定位的元素写 left/top 无效，得走 translate */
			[[nodiscard]] bool IsStatic() const;
			/* 变换属性是否真的挂上去了（调试与检查用） */
			[[nodiscard]] bool HasTransform() const;

			/* 选中元素经「自身 + 祖先」变换后的四角（布局像素、绝对坐标，
			 * 顺序：左上 / 右上 / 右下 / 左下）。父级被移动/旋转/缩放过时选框靠它对位 */
			[[nodiscard]] bool GetRenderQuad(float (&corners)[4][2]) const;
			/* 任意元素的复合四角（自己 + 祖先的平移/旋转/缩放） */
			void GetElementRenderQuad(Rml::Element* element, float (&corners)[4][2]) const;
			/* 祖先链上的位移合计（dp）：父级被拖动过之后子元素渲染位置会整体偏 */
			void GetAncestorTranslate(float& x, float& y) const;

			/* 命中测试：坐标是物理像素，返回光标下的元素与它的盒子 */
			bool HitTest(Rml::Context& context, float x, float y, ElementHit& out) const;

			/* 元素定位：有 id 就用 id（文档重载后仍然有效），没有 id 退回先序下标
			 * （文档不重载时有效）。撤销命令两种都记着，找元素时优先 id */
			struct ElementKey
			{
				std::string Id{};
				int         Index{ -1 };
			};

			/* 选中元素的整份状态（撤销记「改动前」用它） */
			bool GetSelectionState(ElementState& out) const;
			/* 按 key 找元素：撤销与属性写回改的可能不是当前选中的那个 */
			[[nodiscard]] Rml::Element* FindElement(const ElementKey& key) const;
			/* 按 id 找元素：保存与撤销要处理的可能已经不在选中 */
			[[nodiscard]] Rml::Element* FindElementById(const std::string& id) const;
			/* 按 key 把状态写回元素：撤销时不看当前选中是谁。元素不在返回 false */
			bool ApplyState(const ElementKey& key, const ElementState& state);
			/* 按 id 选中（保存重载后恢复用）；元素不在返回 false */
			bool SelectById(const std::string& id);
			/* 选中元素实际的旋转锚点（纹理坐标）：旋转/缩放时画出来 */
			bool GetAnchorPoint(float& x, float& y) const;
			/* 元素自带的变换（matrix / skew / 3D）面板表达不了：显示提示，不硬改 */
			[[nodiscard]] bool IsTransformOpaque() const { return m_TransformOpaque; }

			/* ---- 元素树：结构快照与按行选中 ----
			 * 树按先序排，下标就是「第几个元素」，不依赖有没有 id（没有 id 的元素
			 * 也能在树里选中，只是写不回 .rcss）。文档重开前下标一直有效。 */
			struct ElementNode
			{
				std::string Id{};
				std::string Tag{};
				std::string ClassName{};
				int         Depth{ 0 };
				bool        HasChildren{ false };
				float       X{ 0.0f };
				float       Y{ 0.0f };
				float       Width{ 0.0f };
				float       Height{ 0.0f };
			};

			/* 文档结构快照（先序，文本节点与 head 不进树） */
			void CollectElements(std::vector<ElementNode>& out) const;
			/* 按先序下标选中（树里点行）；下标越界返回 false */
			bool SelectIndex(size_t index);
			/* 当前选中的元素在先序里的下标，没选中返回 -1 */
			[[nodiscard]] int GetSelectionIndex() const;

		private:
			[[nodiscard]] const ElementState* FindState(Rml::Element* element) const;
			/* 选中元素当前的值，合成一份状态 */
			[[nodiscard]] ElementState CurrentState() const;
			/* 按 key 找元素：先认 id，没有 id 再按先序下标 */
			[[nodiscard]] Rml::Element* ResolveKey(const ElementKey& key) const;
			/* 先序排一份元素表（深度一起带上）：树、按行选中、找选中下标共用 */
			void CollectOrder(std::vector<std::pair<Rml::Element*, int>>& out) const;
			/* 锁定一个元素并把记下的状态贴回去（点选与树里点行走同一套） */
			void SelectElement(Rml::Element* element);
			/* 从元素自己的本地变换还原一份状态：重载后状态表是空的，
			 * 但元素上可能还留着 CSS 或上一次写下的 transform */
			bool ReadDomState(Rml::Element* element, ElementState& out) const;
			/* 把平移 / 旋转 / 缩放 / 原点一起写到元素上 */
			void WriteTransform(Rml::Element* element, float translate_x, float translate_y,
				float rotation, float scale, float anchor_x, float anchor_y) const;

			Rml::Element* m_Element{ nullptr };
			/* 当前文档：按 id 找元素用，Reset 时清掉 */
			Rml::ElementDocument* m_Document{ nullptr };
			/* 按元素指针记状态：没有 id 的元素各记各的，不会共用一个槽。
			 * 指针只活到文档重载，Reset 时整表清空 */
			std::map<Rml::Element*, ElementState> m_States{};

			float m_PixelsPerDp{ 1.0f };
			float m_Rotation{ 0.0f };
			float m_Scale{ 1.0f };
			float m_TranslateX{ 0.0f };
			float m_TranslateY{ 0.0f };
			float m_AnchorX{ 0.5f };
			float m_AnchorY{ 0.5f };
			/* 拖动起点：按下时记下 left/top，后续按增量写回 */
			/* 选中元素自带的变换没法用面板的四个数值表达 */
			bool  m_TransformOpaque{ false };
			float m_DragStartLeft{ 0.0f };
			float m_DragStartTop{ 0.0f };
			bool m_Dragging{ false };
		};
	}
}
