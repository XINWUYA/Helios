#include "Pch.h"
#include "PreviewSelection.h"

#include <RmlUi/Core.h>

#include <cstdio>

namespace Helios
{
	namespace UIEditor
	{
		namespace
		{
			/* 文本节点不进树（结构树是给人看的），head 里的样式 / 字体声明同理 */
			bool IsTreeElement(const Rml::Element* element)
			{
				if (element == nullptr)
					return false;

				const std::string tag(element->GetTagName().c_str());
				return tag != "#text" && tag != "head";
			}

			/* 从 "translate(12dp, 8dp) rotate(15deg) scale(1.2)" 里认出面板写得回去的三种变换。
			 * 出现 matrix / skew / perspective 这类写法就认作"表达不了"，返回 false */
			bool ParseTransform(const std::string& text, float& translate_x, float& translate_y,
				float& rotate, float& scale)
			{
				translate_x = 0.0f;
				translate_y = 0.0f;
				rotate = 0.0f;
				scale = 1.0f;

				if (text.empty())
					return true;
				if (text.find("matrix") != std::string::npos || text.find("skew") != std::string::npos
					|| text.find("perspective") != std::string::npos)
					return false;

				if (const size_t at = text.find("rotate("); at != std::string::npos)
					rotate = std::strtof(text.c_str() + at + 7, nullptr);

				if (const size_t at = text.find("scale("); at != std::string::npos)
					scale = std::strtof(text.c_str() + at + 6, nullptr);

				if (const size_t at = text.find("translate("); at != std::string::npos)
				{
					const char* begin = text.c_str() + at + 10;
					char* end = nullptr;
					translate_x = std::strtof(begin, &end);

					/* 第一个分量后面跟着单位（dp / px），要跳过它再找逗号，
					 * 否则第二个分量会被读成 0 —— 元素被拖远之后旋转就会绕错点 */
					if (end != nullptr)
					{
						while (*end != '\0' && *end != ',' && *end != ')')
							++end;
						if (*end == ',')
							++end;
						translate_y = std::strtof(end, nullptr);
					}
				}
				return true;
			}

			/* transform-origin 存的是百分比长写；本地没写就用计算值（样式表里的），再没有用默认值 */
			float ReadOriginPercent(Rml::Element* element, const char* name, float fallback)
			{
				const Rml::Property* property = element->GetLocalProperty(name);
				if (property == nullptr)
					property = element->GetProperty(name);
				if (property == nullptr)
					return fallback;

				const std::string text = property->ToString().c_str();
				if (text.empty() || text.back() != '%')
					return fallback;
				return std::strtof(text.c_str(), nullptr) / 100.0f;
			}

			/* 先序收集元素与深度：树的下标、缩进、按行选中都按这份顺序 */
			void CollectInto(Rml::Element* element, int depth,
				std::vector<std::pair<Rml::Element*, int>>& out)
			{
				if (!IsTreeElement(element))
					return;

				out.emplace_back(element, depth);
				for (int index = 0; index < element->GetNumChildren(); ++index)
					CollectInto(element->GetChild(index), depth + 1, out);
			}
		}

		void PreviewSelection::Reset()
		{
			m_Element = nullptr;
			m_Document = nullptr;
			m_States.clear();
			m_Rotation = 0.0f;
			m_Scale = 1.0f;
			m_TranslateX = 0.0f;
			m_TranslateY = 0.0f;
			m_AnchorX = 0.5f;
			m_AnchorY = 0.5f;
			m_DragStartLeft = 0.0f;
			m_DragStartTop = 0.0f;
			m_Dragging = false;
		}

		void PreviewSelection::SetDensityRatio(float ratio)
		{
			m_PixelsPerDp = ratio > 0.0f ? ratio : 1.0f;
		}

		bool PreviewSelection::SelectAt(Rml::Context& context, Rml::ElementDocument* document, float x, float y)
		{
			m_Document = document;
			Rml::Element* element = context.GetElementAtPoint(Rml::Vector2f(x, y));
			/* 空白处（body / 文档本身）当成取消选中 */
			if (element == nullptr || element == document || element->GetTagName() == "body")
			{
				m_Element = nullptr;
				return false;
			}

			SelectElement(element);
			return true;
		}

		/* 锁定一个元素并把记下的状态贴回去：点选与树里点行走同一套，
		 * 否则从树里选中的元素会缺一份状态，选框对不上 */
		bool PreviewSelection::ReadDomState(Rml::Element* element, ElementState& out) const
		{
			if (element == nullptr)
				return false;

			out = ElementState{};
			out.AnchorX = ReadOriginPercent(element, "transform-origin-x", 0.5f);
			out.AnchorY = ReadOriginPercent(element, "transform-origin-y", 0.5f);

			/* 先看本地声明，再看计算值：写进 .rcss 的变换重载后只剩计算值，
			 * 只读本地的话选框和悬停框都会停在布局位置 */
			const Rml::Property* transform = element->GetLocalProperty("transform");
			if (transform == nullptr)
				transform = element->GetProperty(Rml::PropertyId::Transform);
			if (transform == nullptr)
				return true;   /* 没有变换，就是默认状态 */

			return ParseTransform(transform->ToString().c_str(), out.TranslateX, out.TranslateY,
				out.Rotate, out.Scale);
		}

		void PreviewSelection::SelectElement(Rml::Element* element)
		{
			m_Element = element;
			if (element == nullptr)
				return;

			/* 有记录就用记录；没有就从元素上现有的变换回填 —— 点选本身不再写元素 */
			if (const ElementState* state = FindState(element))
			{
				m_TranslateX = state->TranslateX;
				m_TranslateY = state->TranslateY;
				m_Rotation = state->Rotate;
				m_Scale = state->Scale;
				m_AnchorX = state->AnchorX;
				m_AnchorY = state->AnchorY;
				m_DragStartLeft = state->Left;
				m_DragStartTop = state->Top;
				m_TransformOpaque = false;
				return;
			}

			m_TranslateX = 0.0f;
			m_TranslateY = 0.0f;
			m_Rotation = 0.0f;
			m_Scale = 1.0f;
			m_AnchorX = ReadOriginPercent(element, "transform-origin-x", 0.5f);
			m_AnchorY = ReadOriginPercent(element, "transform-origin-y", 0.5f);

			ElementState dom;
			m_TransformOpaque = !ReadDomState(element, dom);
			if (!m_TransformOpaque)
			{
				m_TranslateX = dom.TranslateX;
				m_TranslateY = dom.TranslateY;
				m_Rotation = dom.Rotate;
				m_Scale = dom.Scale;
				m_AnchorX = dom.AnchorX;
				m_AnchorY = dom.AnchorY;
			}

			m_DragStartLeft = 0.0f;
			m_DragStartTop = 0.0f;
			GetPosition(m_DragStartLeft, m_DragStartTop);
		}

		bool PreviewSelection::GetAnchorPoint(float& x, float& y) const
		{
			if (m_Element == nullptr)
				return false;

			const Rml::Vector2f offset = m_Element->GetAbsoluteOffset(Rml::BoxArea::Border);
			const Rml::Vector2f size = m_Element->GetBox().GetSize(Rml::BoxArea::Border);

			/* 锚点在元素自身坐标里不动，平移把它整体带走；旋转绕它转，所以它自己不受旋转影响 */
			x = offset.x + size.x * m_AnchorX + m_TranslateX * m_PixelsPerDp;
			y = offset.y + size.y * m_AnchorY + m_TranslateY * m_PixelsPerDp;
			return true;
		}

		bool PreviewSelection::SelectById(const std::string& id)
		{
			Rml::Element* element = FindElementById(id);
			if (element == nullptr)
				return false;

			SelectElement(element);
			return true;
		}

		void PreviewSelection::Clear()
		{
			m_Element = nullptr;
			m_TransformOpaque = false;
		}

		std::string PreviewSelection::GetId() const
		{
			return m_Element != nullptr ? std::string(m_Element->GetId().c_str()) : std::string();
		}

		bool PreviewSelection::GetBox(float& x, float& y, float& width, float& height) const
		{
			if (m_Element == nullptr)
				return false;

			const Rml::Vector2f offset = m_Element->GetAbsoluteOffset(Rml::BoxArea::Border);
			const Rml::Vector2f size = m_Element->GetBox().GetSize(Rml::BoxArea::Border);
			x = offset.x;
			y = offset.y;
			width = size.x;
			height = size.y;
			return true;
		}

		bool PreviewSelection::GetPosition(float& left, float& top) const
		{
			if (m_Element == nullptr)
				return false;

			/* 优先用声明值：left/top 是相对包含块的，跟祖先有没有 transform 无关；
			 * auto 时（没有声明）才退回盒子折回 */
			const Rml::ComputedValues& values = m_Element->GetComputedValues();
			const Rml::Style::LengthPercentageAuto declared_left = values.left();
			const Rml::Style::LengthPercentageAuto declared_top = values.top();
			const bool has_left = declared_left.type != Rml::Style::LengthPercentageAuto::Auto;
			const bool has_top = declared_top.type != Rml::Style::LengthPercentageAuto::Auto;

			if (has_left && has_top)
			{
				left = declared_left.value / m_PixelsPerDp;
				top = declared_top.value / m_PixelsPerDp;
				return true;
			}

			const Rml::Vector2f offset = m_Element->GetAbsoluteOffset(Rml::BoxArea::Border);
			Rml::Vector2f origin(0.0f, 0.0f);
			if (Rml::Element* parent = m_Element->GetParentNode())
				origin = parent->GetAbsoluteOffset(Rml::BoxArea::Border);

			left = (offset.x - origin.x) / m_PixelsPerDp;
			top = (offset.y - origin.y) / m_PixelsPerDp;
			return true;
		}

		void PreviewSelection::SetPosition(float left, float top)
		{
			if (m_Element == nullptr)
				return;

			m_Element->SetProperty(Rml::PropertyId::Left, Rml::Property(left, Rml::Unit::DP));
			m_Element->SetProperty(Rml::PropertyId::Top, Rml::Property(top, Rml::Unit::DP));
		}

		void PreviewSelection::BeginDrag()
		{
			m_Dragging = GetPosition(m_DragStartLeft, m_DragStartTop);
		}

		bool PreviewSelection::Drag(float dx, float dy)
		{
			if (!m_Dragging || m_Element == nullptr)
				return false;

			const float delta_x = dx / m_PixelsPerDp;
			const float delta_y = dy / m_PixelsPerDp;

			/* 静态定位（flex 子项、普通流）写 left/top 会被忽略，改走 translate；
			 * 已定位元素仍改 left/top，数值直观、也不经过 transform */
			if (m_Element->GetComputedValues().position() == Rml::Style::Position::Static)
			{
				m_TranslateX += delta_x;
				m_TranslateY += delta_y;
				SetTransform(m_Rotation, m_Scale);
				return true;
			}

			m_DragStartLeft += delta_x;
			m_DragStartTop += delta_y;
			m_Element->SetProperty(Rml::PropertyId::Left, Rml::Property(m_DragStartLeft, Rml::Unit::DP));
			m_Element->SetProperty(Rml::PropertyId::Top, Rml::Property(m_DragStartTop, Rml::Unit::DP));
			return true;
		}

		void PreviewSelection::EndDrag()
		{
			m_Dragging = false;

			/* 一次性把当前状态写回表里：位移、旋转、缩放、锚点都按 id 留着 */
			if (m_Element == nullptr)
				return;

			ElementState state = CurrentState();
			GetPosition(state.Left, state.Top);
			m_States[m_Element] = state;
		}

		void PreviewSelection::SetTransform(float rotation_degrees, float scale)
		{
			m_Rotation = rotation_degrees;
			m_Scale = scale;
			if (m_Element == nullptr)
				return;

			WriteTransform(m_Element, m_TranslateX, m_TranslateY, rotation_degrees, scale,
				m_AnchorX, m_AnchorY);
		}

		void PreviewSelection::WriteTransform(Rml::Element* element, float translate_x, float translate_y,
			float rotation, float scale, float anchor_x, float anchor_y) const
		{
			if (element == nullptr)
				return;

			/* 显式构造 PrimitiveList：花括号初始化在自有 Vector 上不保证走到正确的重载 */
			Rml::Transform::PrimitiveList primitives;
			if (translate_x != 0.0f || translate_y != 0.0f)
				primitives.push_back(Rml::Transforms::Translate2D(translate_x, translate_y, Rml::Unit::DP));
			primitives.push_back(Rml::Transforms::Rotate2D(rotation));
			primitives.push_back(Rml::Transforms::Scale2D(scale));

			const Rml::Property property = Rml::Transform::MakeProperty(std::move(primitives));
			element->SetProperty(Rml::PropertyId::Transform, property);

			/* 原点跟变换一起写：只用字符串形式，避免两分量属性的构造细节 */
			char origin[64];
			std::snprintf(origin, sizeof(origin), "%.1f%% %.1f%%",
				static_cast<double>(anchor_x * 100.0f), static_cast<double>(anchor_y * 100.0f));
			element->SetProperty("transform-origin", origin);
		}

		void PreviewSelection::SetTransformState(float rotation_degrees, float scale)
		{
			m_Rotation = rotation_degrees;
			m_Scale = scale;
		}

		void PreviewSelection::SetTranslate(float x, float y)
		{
			m_TranslateX = x;
			m_TranslateY = y;
			if (m_Element != nullptr)
				SetTransform(m_Rotation, m_Scale);
		}

		void PreviewSelection::GetTranslate(float& x, float& y) const
		{
			x = m_TranslateX;
			y = m_TranslateY;
		}

		void PreviewSelection::SetAnchor(float x, float y)
		{
			m_AnchorX = std::clamp(x, 0.0f, 1.0f);
			m_AnchorY = std::clamp(y, 0.0f, 1.0f);
		}

		bool PreviewSelection::IsStatic() const
		{
			return m_Element != nullptr
				&& m_Element->GetComputedValues().position() == Rml::Style::Position::Static;
		}

		bool PreviewSelection::HasTransform() const
		{
			/* 只看本地声明：GetProperty 连默认值一起返回，判断不了"有没有写过" */
			return m_Element != nullptr && m_Element->GetLocalProperty("transform") != nullptr;
		}

		bool PreviewSelection::GetRenderQuad(float (&corners)[4][2]) const
		{
			if (m_Element == nullptr)
				return false;
			GetElementRenderQuad(m_Element, corners);
			return true;
		}

		void PreviewSelection::GetElementRenderQuad(Rml::Element* element, float (&corners)[4][2]) const
		{
			if (element == nullptr)
				return;

			const Rml::Vector2f offset = element->GetAbsoluteOffset(Rml::BoxArea::Border);
			const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);

			/* 依次作用：自身变换 → 每层祖先变换（由内往外）。
			 * 顺序要跟写进元素的 `translate(...) rotate(...) scale(...)` 一致：
			 * CSS 从右往左生效，也就是先缩放、绕原点旋转，最后整体平移。
			 * 把平移放在旋转之前算，元素被拖远之后选框会偏出同样的距离 */
			const auto apply = [](float x, float y, float centre_x, float centre_y,
				float translate_x, float translate_y, float rotate_degrees, float scale)
			{
				const float radians = rotate_degrees * 3.14159265f / 180.0f;
				const float cos_value = std::cos(radians);
				const float sin_value = std::sin(radians);
				const float local_x = (x - centre_x) * scale;
				const float local_y = (y - centre_y) * scale;
				x = centre_x + local_x * cos_value - local_y * sin_value + translate_x;
				y = centre_y + local_x * sin_value + local_y * cos_value + translate_y;
				return std::pair<float, float>{ x, y };
			};

			float points[4][2] = {
				{ offset.x, offset.y },
				{ offset.x + size.x, offset.y },
				{ offset.x + size.x, offset.y + size.y },
				{ offset.x, offset.y + size.y },
			};

			/* 自身：选中元素用面板状态；其它元素优先用记录，没有就按元素上的本地变换。
			 * 少了最后这层，重载后（状态表已清）选框会脱离元素 */
			ElementState self;
			if (element == m_Element)
			{
				self = CurrentState();
			}
			else if (const ElementState* stored = FindState(element))
			{
				self = *stored;
			}
			else
			{
				ReadDomState(element, self);
			}
			{
				const float centre_x = offset.x + size.x * self.AnchorX;
				const float centre_y = offset.y + size.y * self.AnchorY;
				for (auto& point : points)
				{
					const auto moved = apply(point[0], point[1], centre_x, centre_y,
						self.TranslateX * m_PixelsPerDp, self.TranslateY * m_PixelsPerDp, self.Rotate, self.Scale);
					point[0] = moved.first;
					point[1] = moved.second;
				}
			}

			/* 祖先：逐层套用，锚点用各自盒子的原点 + 比例。同样会退回元素上的本地变换 */
			for (Rml::Element* parent = element->GetParentNode(); parent != nullptr;
				parent = parent->GetParentNode())
			{
				ElementState state;
				if (const ElementState* stored = FindState(parent))
					state = *stored;
				else if (!ReadDomState(parent, state))
					continue;

				if (state.TranslateX == 0.0f && state.TranslateY == 0.0f
					&& state.Rotate == 0.0f && state.Scale == 1.0f)
					continue;

				const Rml::Vector2f parent_offset = parent->GetAbsoluteOffset(Rml::BoxArea::Border);
				const Rml::Vector2f parent_size = parent->GetBox().GetSize(Rml::BoxArea::Border);
				const float centre_x = parent_offset.x + parent_size.x * state.AnchorX;
				const float centre_y = parent_offset.y + parent_size.y * state.AnchorY;

				for (auto& point : points)
				{
					const auto moved = apply(point[0], point[1], centre_x, centre_y,
						state.TranslateX * m_PixelsPerDp, state.TranslateY * m_PixelsPerDp,
						state.Rotate, state.Scale);
					point[0] = moved.first;
					point[1] = moved.second;
				}
			}

			for (int index = 0; index < 4; ++index)
			{
				corners[index][0] = points[index][0];
				corners[index][1] = points[index][1];
			}
		}

		void PreviewSelection::GetAncestorTranslate(float& x, float& y) const
		{
			x = 0.0f;
			y = 0.0f;
			if (m_Element == nullptr)
				return;

			for (Rml::Element* parent = m_Element->GetParentNode(); parent != nullptr;
				parent = parent->GetParentNode())
			{
				const ElementState* state = FindState(parent);
				if (state == nullptr)
					continue;
				x += state->TranslateX;
				y += state->TranslateY;
			}
		}

		bool PreviewSelection::HitTest(Rml::Context& context, float x, float y, ElementHit& out) const
		{
			Rml::Element* element = context.GetElementAtPoint(Rml::Vector2f(x, y));
			if (element == nullptr)
				return false;

			out.Id = element->GetId().c_str();
			out.ClassName = element->GetClassNames().c_str();

			const Rml::Vector2f offset = element->GetAbsoluteOffset(Rml::BoxArea::Border);
			const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
			out.X = offset.x;
			out.Y = offset.y;
			out.Width = size.x;
			out.Height = size.y;

			/* 和父级的盒子比一比：超出就是布局写错（宽度没定、flex 没换行这类） */
			if (Rml::Element* parent = element->GetParentNode())
			{
				const Rml::Vector2f parent_offset = parent->GetAbsoluteOffset(Rml::BoxArea::Border);
				const Rml::Vector2f parent_size = parent->GetBox().GetSize(Rml::BoxArea::Border);
				out.OverflowX = out.X + out.Width > parent_offset.x + parent_size.x + 0.5f
					|| out.X < parent_offset.x - 0.5f;
				out.OverflowY = out.Y + out.Height > parent_offset.y + parent_size.y + 0.5f
					|| out.Y < parent_offset.y - 0.5f;
			}

			GetElementRenderQuad(element, out.Corners);
			return true;
		}

		const PreviewSelection::ElementState* PreviewSelection::FindState(Rml::Element* element) const
		{
			if (element == nullptr)
				return nullptr;

			const auto found = m_States.find(element);
			return found != m_States.end() ? &found->second : nullptr;
		}

		PreviewSelection::ElementState PreviewSelection::CurrentState() const
		{
			ElementState state;
			state.TranslateX = m_TranslateX;
			state.TranslateY = m_TranslateY;
			state.Rotate = m_Rotation;
			state.Scale = m_Scale;
			state.AnchorX = m_AnchorX;
			state.AnchorY = m_AnchorY;
			return state;
		}

		bool PreviewSelection::GetSelectionState(ElementState& out) const
		{
			if (m_Element == nullptr)
				return false;

			out = CurrentState();
			GetPosition(out.Left, out.Top);
			return true;
		}

		Rml::Element* PreviewSelection::FindElementById(const std::string& id) const
		{
			if (m_Document == nullptr || id.empty())
				return nullptr;
			return m_Document->GetElementById(Rml::String(id.c_str()));
		}

		Rml::Element* PreviewSelection::FindElement(const ElementKey& key) const
		{
			return ResolveKey(key);
		}

		Rml::Element* PreviewSelection::ResolveKey(const ElementKey& key) const
		{
			/* 先认 id：文档重载后指针全换了，id 才是稳的 */
			if (!key.Id.empty())
				return FindElementById(key.Id);

			if (key.Index < 0)
				return nullptr;

			std::vector<std::pair<Rml::Element*, int>> order;
			CollectOrder(order);
			const size_t at = static_cast<size_t>(key.Index);
			return at < order.size() ? order[at].first : nullptr;
		}

		bool PreviewSelection::ApplyState(const ElementKey& key, const ElementState& state)
		{
			Rml::Element* element = ResolveKey(key);
			if (element == nullptr)
				return false;

			/* 与拖动时同一套判定：定位元素写 left/top，静态元素只走 translate */
			if (element->GetComputedValues().position() != Rml::Style::Position::Static)
			{
				element->SetProperty(Rml::PropertyId::Left, Rml::Property(state.Left, Rml::Unit::DP));
				element->SetProperty(Rml::PropertyId::Top, Rml::Property(state.Top, Rml::Unit::DP));
			}

			WriteTransform(element, state.TranslateX, state.TranslateY, state.Rotate, state.Scale,
				state.AnchorX, state.AnchorY);

			/* 状态表跟着更新：下次点回这个元素看到的是撤销后的值 */
			m_States[element] = state;
			if (element == m_Element)
			{
				m_Rotation = state.Rotate;
				m_Scale = state.Scale;
				m_TranslateX = state.TranslateX;
				m_TranslateY = state.TranslateY;
				m_AnchorX = state.AnchorX;
				m_AnchorY = state.AnchorY;
			}
			return true;
		}

		void PreviewSelection::CollectOrder(std::vector<std::pair<Rml::Element*, int>>& out) const
		{
			out.clear();
			CollectInto(m_Document, 0, out);
		}

		void PreviewSelection::CollectElements(std::vector<ElementNode>& out) const
		{
			out.clear();

			std::vector<std::pair<Rml::Element*, int>> order;
			CollectOrder(order);
			out.reserve(order.size());

			for (const auto& [element, depth] : order)
			{
				ElementNode node;
				node.Id = element->GetId().c_str();
				node.Tag = element->GetTagName().c_str();
				node.ClassName = element->GetClassNames().c_str();
				node.Depth = depth;

				/* 展开箭头按「树里真的看得见子元素」算：只有文本的节点不该给箭头 */
				for (int index = 0; index < element->GetNumChildren(); ++index)
				{
					if (IsTreeElement(element->GetChild(index)))
					{
						node.HasChildren = true;
						break;
					}
				}

				const Rml::Vector2f offset = element->GetAbsoluteOffset(Rml::BoxArea::Border);
				const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
				node.X = offset.x;
				node.Y = offset.y;
				node.Width = size.x;
				node.Height = size.y;
				out.push_back(std::move(node));
			}
		}

		bool PreviewSelection::SelectIndex(size_t index)
		{
			std::vector<std::pair<Rml::Element*, int>> order;
			CollectOrder(order);
			if (index >= order.size())
				return false;

			Rml::Element* element = order[index].first;
			/* 文档本体与 body 当空白处理，和画布上点空白一致 */
			if (element == nullptr || element == m_Document || element->GetTagName() == "body")
			{
				m_Element = nullptr;
				return false;
			}

			SelectElement(element);
			return true;
		}

		int PreviewSelection::GetSelectionIndex() const
		{
			if (m_Element == nullptr)
				return -1;

			std::vector<std::pair<Rml::Element*, int>> order;
			CollectOrder(order);
			for (size_t index = 0; index < order.size(); ++index)
			{
				if (order[index].first == m_Element)
					return static_cast<int>(index);
			}
			return -1;
		}
	}
}
