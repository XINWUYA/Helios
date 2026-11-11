#include "Pch.h"
#include "CssPatch.h"

#include <algorithm>

namespace Helios
{
	namespace UIEditor
	{
		namespace
		{
			bool IsSpace(char character)
			{
				return character == ' ' || character == '\t' || character == '\r' || character == '\n';
			}

			/* 找 `#id { ... }` 的块边界。复合选择器（`.btn:hover`）和以该 id 开头的
			 * 其它选择器都不算，规则与改值那套一致 */
			bool FindBlock(const std::string& text, const std::string& id,
				size_t& out_open, size_t& out_close)
			{
				const std::string selector = "#" + id;
				size_t search = 0;
				while (true)
				{
					const size_t found = text.find(selector, search);
					if (found == std::string::npos)
						return false;

					search = found + selector.size();
					size_t cursor = found + selector.size();
					while (cursor < text.size() && IsSpace(text[cursor]))
						++cursor;
					if (cursor >= text.size() || text[cursor] != '{')
						continue;

					size_t depth = 0;
					for (size_t index = cursor; index < text.size(); ++index)
					{
						if (text[index] == '{')
							++depth;
						else if (text[index] == '}')
						{
							if (--depth == 0)
							{
								out_open = cursor;
								out_close = index;
								return true;
							}
						}
					}
					return false;
				}
			}

			/* 块里找 `property:` 的冒号位置（跳过注释与已有声明之间的分号） */
			size_t FindDeclaration(const std::string& text, size_t block_open, size_t block_close,
				const std::string& property)
			{
				size_t scan = block_open + 1;
				bool in_comment = false;
				while (scan < block_close)
				{
					if (!in_comment && text[scan] == '/' && scan + 1 < block_close && text[scan + 1] == '*')
					{
						in_comment = true;
						scan += 2;
						continue;
					}
					if (in_comment)
					{
						if (text[scan] == '*' && scan + 1 < block_close && text[scan + 1] == '/')
						{
							in_comment = false;
							scan += 2;
							continue;
						}
						++scan;
						continue;
					}

					const size_t name_start = scan;
					while (scan < block_close && text[scan] != ':' && text[scan] != ';' && text[scan] != '}')
						++scan;

					if (scan < block_close && text[scan] == ':')
					{
						std::string name = text.substr(name_start, scan - name_start);
						name.erase(std::remove_if(name.begin(), name.end(), IsSpace), name.end());
						if (name == property)
							return scan;
					}

					while (scan < block_close && text[scan] != ';')
						++scan;
					++scan;
				}
				return std::string::npos;
			}
		}

		bool PatchCssDeclaration(std::string& text, const std::string& id, const std::string& property,
			const std::string& value, bool& out_found)
		{
			out_found = false;
			if (id.empty() || property.empty())
				return false;

			size_t block_open = 0;
			size_t block_close = 0;
			if (!FindBlock(text, id, block_open, block_close))
				return false;

			out_found = true;

			const size_t colon_at = FindDeclaration(text, block_open, block_close, property);
			if (colon_at != std::string::npos)
			{
				/* 已有属性：把值换掉，保留原来的 `;` */
				size_t value_start = colon_at + 1;
				while (value_start < block_close && IsSpace(text[value_start]))
					++value_start;
				size_t value_end = value_start;
				while (value_end < block_close && text[value_end] != ';')
					++value_end;

				if (text.substr(value_start, value_end - value_start) == value)
					return false;   /* 值没变 */

				text.replace(value_start, value_end - value_start, value);
				return true;
			}

			/* 没有这条属性：在块尾插入，缩进跟块内第一行对齐 */
			size_t indent_start = block_close;
			while (indent_start > block_open + 1 && text[indent_start - 1] != '\n')
				--indent_start;

			std::string indentation;
			size_t indent_scan = indent_start;
			while (indent_scan < block_close && (text[indent_scan] == '\t' || text[indent_scan] == ' '))
			{
				indentation.push_back(text[indent_scan]);
				++indent_scan;
			}
			if (indentation.empty())
				indentation = "\t";

			const std::string inserted = indentation + property + ": " + value + ";\n";
			text.insert(indent_start, inserted);
			return true;
		}

		bool RemoveCssDeclaration(std::string& text, const std::string& id, const std::string& property,
			bool& out_found)
		{
			out_found = false;
			if (id.empty() || property.empty())
				return false;

			size_t block_open = 0;
			size_t block_close = 0;
			if (!FindBlock(text, id, block_open, block_close))
				return false;

			out_found = true;

			const size_t colon_at = FindDeclaration(text, block_open, block_close, property);
			if (colon_at == std::string::npos)
				return false;   /* 本来就没这条：视为已达成 */

			/* 整条声明连同前面的空白一起删：从行首（或分号之后）到分号后 */
			size_t begin = colon_at;
			while (begin > block_open + 1 && text[begin - 1] != ';' && text[begin - 1] != '{')
				--begin;

			size_t end = colon_at;
			while (end < block_close && text[end] != ';')
				++end;
			if (end < block_close)
				++end;   /* 吃掉分号 */

			/* 顺带吃掉这一行剩下的空白和一个换行，免得留下空行 */
			while (end < block_close && (text[end] == ' ' || text[end] == '\t'))
				++end;
			if (end < block_close && text[end] == '\n')
				++end;

			text.erase(begin, end - begin);
			return true;
		}

		bool AppendCssBlock(std::string& text, const std::string& id,
			const std::vector<std::pair<std::string, std::string>>& declarations)
		{
			if (id.empty() || declarations.empty())
				return false;

			size_t block_open = 0;
			size_t block_close = 0;
			if (FindBlock(text, id, block_open, block_close))
				return false;   /* 已经有块了：交给改值那条路 */

			if (!text.empty() && text.back() != '\n')
				text.push_back('\n');
			text += "\n#" + id + "\n{\n";
			for (const auto& [property, value] : declarations)
				text += "\t" + property + ": " + value + ";\n";
			text += "}\n";
			return true;
		}
	}
}
