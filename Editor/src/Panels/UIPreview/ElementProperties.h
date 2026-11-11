#pragma once
#include <string>
#include <vector>

namespace Rml
{
	class Element;
}

namespace Helios::UIEditor
{
	/* 属性的落点：inline 样式、元素属性、元素里的文本 */
	enum class ElementPropertyKind
	{
		Style,
		Attribute,
		Text,
	};

	/* 面板用什么控件画这一行 */
	enum class ElementPropertyEditor
	{
		Text,    /* 单行输入 */
		Area,    /* 多行输入：元素文本 */
		Color,   /* 色块 + 十六进制文本 */
		Choice,  /* 下拉，候选见 Choices */
	};

	/* 属性只对某类元素有意义时在这里声明 */
	enum class ElementPropertyScope
	{
		Any,
		TextContent,  /* 只含文本的元素：按钮文字、标题这类 */
		Image,        /* img 的 src */
	};

	struct ElementPropertyDesc
	{
		const char*           Category;
		const char*           Label;
		const char*           Key;   /* CSS 属性名 / 元素属性名；Text 类为空 */
		ElementPropertyKind   Kind;
		ElementPropertyEditor Editor;
		ElementPropertyScope  Scope;
		const char* const*    Choices; /* Choice 用，空指针结尾 */
		/* 只读：id / class 这类改了就断锚点或命中集合，先只显示 */
		bool                  ReadOnly{ false };
	};

	struct ElementPropertyValue
	{
		const ElementPropertyDesc* Desc{ nullptr };
		std::string                Value{};
	};

	/* 这个元素用得上的属性，顺序即面板顺序（分类看 Desc->Category） */
	void CollectElementProperties(Rml::Element* element, std::vector<ElementPropertyValue>& out);

	/* 读 / 写单个属性；写空串等于删掉这条声明 */
	bool ReadElementProperty(Rml::Element* element, const ElementPropertyDesc& desc, std::string& out);
	bool WriteElementProperty(Rml::Element* element, const ElementPropertyDesc& desc,
		const std::string& value);
}
