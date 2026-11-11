#include "Pch.h"
#include "ElementProperties.h"
#include "RmlPatch.h"

#include <RmlUi/Core.h>

namespace Helios::UIEditor
{
	namespace
	{
		const char* const kDisplayChoices[] = { "block", "inline", "inline-block", "flex", "none", nullptr };
		const char* const kFontWeightChoices[] = { "normal", "bold", nullptr };
		const char* const kTextAlignChoices[] = { "left", "center", "right", nullptr };

		/* 表的顺序就是面板里卡片的顺序：高频的 Layout 排最前，
		 * Transform 不是 RmlUi 属性、由面板单独插在最上面 */
		constexpr ElementPropertyDesc kProperties[] = {
			{ "Layout", "Width", "width", ElementPropertyKind::Style,
				ElementPropertyEditor::Text, ElementPropertyScope::Any, nullptr },
			{ "Layout", "Height", "height", ElementPropertyKind::Style,
				ElementPropertyEditor::Text, ElementPropertyScope::Any, nullptr },
			{ "Layout", "Display", "display", ElementPropertyKind::Style,
				ElementPropertyEditor::Choice, ElementPropertyScope::Any, kDisplayChoices },
			{ "Layout", "Margin", "margin", ElementPropertyKind::Style,
				ElementPropertyEditor::Text, ElementPropertyScope::Any, nullptr },
			{ "Layout", "Padding", "padding", ElementPropertyKind::Style,
				ElementPropertyEditor::Text, ElementPropertyScope::Any, nullptr },

			{ "Content", "Text", "", ElementPropertyKind::Text,
				ElementPropertyEditor::Area, ElementPropertyScope::TextContent, nullptr },
			{ "Content", "Source", "src", ElementPropertyKind::Attribute,
				ElementPropertyEditor::Text, ElementPropertyScope::Image, nullptr },

			/* color 只影响文字，元素本身的底色是 background-color —— 名字写清免得混淆 */
			{ "Appearance", "Text Color", "color", ElementPropertyKind::Style,
				ElementPropertyEditor::Color, ElementPropertyScope::Any, nullptr },
			{ "Appearance", "Background", "background-color", ElementPropertyKind::Style,
				ElementPropertyEditor::Color, ElementPropertyScope::Any, nullptr },
			/* 外圈那条线是 border：颜色和粗细各一行，圆角紧挨着它们 */
			{ "Appearance", "Border Color", "border-color", ElementPropertyKind::Style,
				ElementPropertyEditor::Color, ElementPropertyScope::Any, nullptr },
			{ "Appearance", "Border Width", "border-width", ElementPropertyKind::Style,
				ElementPropertyEditor::Text, ElementPropertyScope::Any, nullptr },
			{ "Appearance", "Border Radius", "border-radius", ElementPropertyKind::Style,
				ElementPropertyEditor::Text, ElementPropertyScope::Any, nullptr },
			{ "Appearance", "Font Size", "font-size", ElementPropertyKind::Style,
				ElementPropertyEditor::Text, ElementPropertyScope::Any, nullptr },
			{ "Appearance", "Font Weight", "font-weight", ElementPropertyKind::Style,
				ElementPropertyEditor::Choice, ElementPropertyScope::Any, kFontWeightChoices },
			{ "Appearance", "Text Align", "text-align", ElementPropertyKind::Style,
				ElementPropertyEditor::Choice, ElementPropertyScope::Any, kTextAlignChoices },
			{ "Appearance", "Opacity", "opacity", ElementPropertyKind::Style,
				ElementPropertyEditor::Text, ElementPropertyScope::Any, nullptr },

			/* id 是撤销与写回的锚点，class 决定 CSS 命中：先只读，改名另做 */
			{ "Element", "ID", "id", ElementPropertyKind::Attribute,
				ElementPropertyEditor::Text, ElementPropertyScope::Any, nullptr, true },
			{ "Element", "Class", "class", ElementPropertyKind::Attribute,
				ElementPropertyEditor::Text, ElementPropertyScope::Any, nullptr, true },
		};

		std::string UnescapeRml(const std::string& text)
		{
			std::string out;
			out.reserve(text.size());
			for (size_t index = 0; index < text.size(); ++index)
			{
				if (text[index] != '&')
				{
					out.push_back(text[index]);
					continue;
				}

				const size_t end = text.find(';', index);
				if (end == std::string::npos || end - index > 6)
				{
					out.push_back(text[index]);
					continue;
				}

				const std::string entity = text.substr(index + 1, end - index - 1);
				if (entity == "amp") out.push_back('&');
				else if (entity == "lt") out.push_back('<');
				else if (entity == "gt") out.push_back('>');
				else if (entity == "quot") out.push_back('"');
				else if (entity == "apos") out.push_back('\'');
				else
				{
					out.append(text, index, end - index + 1);
					index = end;
					continue;
				}
				index = end;
			}
			return out;
		}

		/* RmlUi 把 margin / padding / border-* 这类简写展开成具体属性，
		 * 本地声明里查不到原名：读值取展开后的第一条，清空要逐条删。
		 * 写入仍用简写原名，多值写法（10dp 20dp）交给 RmlUi 自己拆 */
		const char* const* ShorthandKeys(const std::string& key)
		{
			static const char* const kMargin[] = { "margin-top", "margin-right", "margin-bottom", "margin-left", nullptr };
			static const char* const kPadding[] = { "padding-top", "padding-right", "padding-bottom", "padding-left", nullptr };
			static const char* const kBorderColor[] = { "border-top-color", "border-right-color",
				"border-bottom-color", "border-left-color", nullptr };
			static const char* const kBorderWidth[] = { "border-top-width", "border-right-width",
				"border-bottom-width", "border-left-width", nullptr };
			static const char* const kBorderRadius[] = { "border-top-left-radius", "border-top-right-radius",
				"border-bottom-right-radius", "border-bottom-left-radius", nullptr };

			if (key == "margin")
				return kMargin;
			if (key == "padding")
				return kPadding;
			if (key == "border-color")
				return kBorderColor;
			if (key == "border-width")
				return kBorderWidth;
			if (key == "border-radius")
				return kBorderRadius;
			return nullptr;
		}

		/* 只装文本的元素才能改文本：混着子元素的话，整段替换会把结构冲掉。
		 * 没有子节点的空元素也不给 —— 自闭合标签（<knob />）同样长这样，改了写不回文件 */
		bool IsTextContainer(Rml::Element* element)
		{
			if (element == nullptr || element->IsReplaced() || element->GetNumChildren() == 0)
				return false;

			for (int index = 0; index < element->GetNumChildren(); ++index)
			{
				const Rml::Element* child = element->GetChild(index);
				if (child == nullptr || child->GetTagName() != "#text")
					return false;
			}
			return true;
		}

		bool MatchesScope(Rml::Element* element, ElementPropertyScope scope)
		{
			switch (scope)
			{
			case ElementPropertyScope::Any:
				return true;
			case ElementPropertyScope::TextContent:
				return IsTextContainer(element);
			case ElementPropertyScope::Image:
				return element != nullptr && element->GetTagName() == "img";
			}
			return false;
		}
	}

	void CollectElementProperties(Rml::Element* element, std::vector<ElementPropertyValue>& out)
	{
		out.clear();
		if (element == nullptr)
			return;

		for (const ElementPropertyDesc& desc : kProperties)
		{
			if (!MatchesScope(element, desc.Scope))
				continue;

			ElementPropertyValue value;
			value.Desc = &desc;
			ReadElementProperty(element, desc, value.Value);
			out.push_back(std::move(value));
		}
	}

	bool ReadElementProperty(Rml::Element* element, const ElementPropertyDesc& desc, std::string& out)
	{
		out.clear();
		if (element == nullptr)
			return false;

		switch (desc.Kind)
		{
		case ElementPropertyKind::Text:
			out = UnescapeRml(element->GetInnerRML());
			return true;

		case ElementPropertyKind::Attribute:
		{
			const Rml::Variant* attribute = element->GetAttribute(desc.Key);
			if (attribute == nullptr)
				return false;
			out = attribute->Get<Rml::String>();
			return true;
		}

		case ElementPropertyKind::Style:
		{
			const char* const* expanded = ShorthandKeys(desc.Key);
			const Rml::Property* property = element->GetProperty(expanded != nullptr ? expanded[0] : desc.Key);
			if (property == nullptr)
				return false;
			out = property->ToString();
			return true;
		}
		}
		return false;
	}

	bool WriteElementProperty(Rml::Element* element, const ElementPropertyDesc& desc,
		const std::string& value)
	{
		if (element == nullptr)
			return false;

		switch (desc.Kind)
		{
		case ElementPropertyKind::Text:
			element->SetInnerRML(EscapeRmlText(value));
			return true;

		case ElementPropertyKind::Attribute:
			if (value.empty())
				element->RemoveAttribute(desc.Key);
			else
				element->SetAttribute(desc.Key, Rml::String(value));
			return true;

		case ElementPropertyKind::Style:
		{
			const char* const* expanded = ShorthandKeys(desc.Key);

			/* 空值 = 把这条声明删掉，元素回到样式表给的值 */
			if (value.empty())
			{
				if (expanded != nullptr)
				{
					for (int index = 0; expanded[index] != nullptr; ++index)
						element->RemoveProperty(expanded[index]);
				}
				else
				{
					element->RemoveProperty(desc.Key);
				}
				return true;
			}

			/* RmlUi 对解析不了的值也返回 true（比如少写一位的颜色），
			 * 只看它有没有真把这条本地声明记下来；简写查展开后的第一条 */
			element->SetProperty(desc.Key, Rml::String(value));
			return element->GetLocalProperty(expanded != nullptr ? expanded[0] : desc.Key) != nullptr;
		}
		}
		return false;
	}
}
