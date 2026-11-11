#include "Pch.h"
#include "RmlPatch.h"

#include <cctype>

namespace Helios::UIEditor
{
	std::string EscapeRmlText(const std::string& text)
	{
		std::string out;
		out.reserve(text.size());
		for (const char character : text)
		{
			switch (character)
			{
			case '&': out += "&amp;"; break;
			case '<': out += "&lt;"; break;
			case '>': out += "&gt;"; break;
			case '"': out += "&quot;"; break;
			default: out.push_back(character); break;
			}
		}
		return out;
	}

	namespace
	{
		bool IsNameChar(char character)
		{
			return std::isalnum(static_cast<unsigned char>(character)) != 0
				|| character == '-' || character == '_';
		}

		/* 找 id 对应的开标签：tag_begin 指 '<'，tag_end 指 '>'，tag_name 是标签名 */
		bool FindOpenTag(const std::string& text, const std::string& id, size_t& tag_begin,
			size_t& tag_end, std::string& tag_name)
		{
			if (id.empty())
				return false;

			for (const char quote : { '"', '\'' })
			{
				const std::string pattern = "id=" + std::string(1, quote) + id + quote;
				size_t search = 0;
				while (true)
				{
					const size_t found = text.find(pattern, search);
					if (found == std::string::npos)
						break;
					search = found + pattern.size();

					/* 前面得是空白或别的边界：别把 apid="x" 认成 id="x" */
					if (found > 0 && IsNameChar(text[found - 1]))
						continue;

					const size_t open = text.rfind('<', found);
					if (open == std::string::npos)
						continue;

					/* 从 '<' 往后找标签结束，引号里的 > 不算 */
					size_t cursor = open + 1;
					char current_quote = '\0';
					while (cursor < text.size())
					{
						const char character = text[cursor];
						if (current_quote != '\0')
						{
							if (character == current_quote)
								current_quote = '\0';
						}
						else if (character == '"' || character == '\'')
						{
							current_quote = character;
						}
						else if (character == '>')
						{
							break;
						}
						++cursor;
					}
					if (cursor >= text.size())
						continue;

					size_t name_end = open + 1;
					while (name_end < cursor && !std::isspace(static_cast<unsigned char>(text[name_end]))
						&& text[name_end] != '/' && text[name_end] != '>')
						++name_end;

					tag_name = text.substr(open + 1, name_end - open - 1);
					tag_begin = open;
					tag_end = cursor;
					return !tag_name.empty();
				}
			}
			return false;
		}
	}

	bool PatchRmlAttribute(std::string& text, const std::string& id, const std::string& name,
		const std::string& value, bool& out_found)
	{
		out_found = false;

		size_t tag_begin = 0;
		size_t tag_end = 0;
		std::string tag_name;
		if (!FindOpenTag(text, id, tag_begin, tag_end, tag_name))
			return false;

		out_found = true;

		/* 文件里存的是转义后的形式，比较与写入都按转义后的来 */
		const std::string escaped = EscapeRmlText(value);

		/* 标签里已经有这条属性：换掉引号里的值 */
		for (const char quote : { '"', '\'' })
		{
			const std::string pattern = name + "=" + std::string(1, quote);
			const size_t found = text.find(pattern, tag_begin + 1);
			if (found == std::string::npos || found > tag_end)
				continue;
			if (IsNameChar(text[found - 1]))
				continue;

			const size_t value_begin = found + pattern.size();
			const size_t value_end = text.find(quote, value_begin);
			if (value_end == std::string::npos || value_end > tag_end)
				continue;

			if (text.compare(value_begin, value_end - value_begin, escaped) == 0)
				return false;

			text.replace(value_begin, value_end - value_begin, escaped);
			return true;
		}

		/* 没有这条属性：补在标签末尾，自闭合的 / 之前 */
		size_t insert_at = tag_end;
		if (insert_at > tag_begin && text[insert_at - 1] == '/')
			--insert_at;
		while (insert_at > tag_begin && std::isspace(static_cast<unsigned char>(text[insert_at - 1])))
			--insert_at;

		text.insert(insert_at, " " + name + "=\"" + escaped + "\"");
		return true;
	}

	bool PatchRmlText(std::string& text, const std::string& id, const std::string& content,
		bool& out_found)
	{
		out_found = false;

		size_t tag_begin = 0;
		size_t tag_end = 0;
		std::string tag_name;
		if (!FindOpenTag(text, id, tag_begin, tag_end, tag_name))
			return false;

		out_found = true;

		/* 自闭合标签没有内容可换 */
		if (text[tag_end - 1] == '/')
			return false;

		const std::string closing = "</" + tag_name + ">";
		const size_t content_begin = tag_end + 1;
		const size_t content_end = text.find(closing, content_begin);
		if (content_end == std::string::npos)
			return false;

		/* 内容按 RML 源文本存：与元素 SetInnerRML 用同一套转义 */
		const std::string escaped = EscapeRmlText(content);
		if (text.compare(content_begin, content_end - content_begin, escaped) == 0)
			return false;

		text.replace(content_begin, content_end - content_begin, escaped);
		return true;
	}
}
