#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

/* GLSL 声明扫描：反射和默认值解析原来各用一条 std::regex 扫全文，libc++ 一条就要上百毫秒。
 * 这里把「找 uniform 声明」收成一份手写扫描，规则与原来的正则等价。 */
namespace Helios::GlslScan
{
	inline bool IsIdentStart(char c)
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
	}

	inline bool IsIdentChar(char c)
	{
		return IsIdentStart(c) || (c >= '0' && c <= '9');
	}

	inline bool IsSpace(char c)
	{
		return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
	}

	inline size_t SkipSpaces(const std::string& source, size_t pos)
	{
		while (pos < source.size() && IsSpace(source[pos]))
			++pos;
		return pos;
	}

	/* 只跳空格和制表符（对应正则里行尾的 [\r\t ]*） */
	inline size_t SkipInlineSpaces(const std::string& source, size_t pos)
	{
		while (pos < source.size() && (source[pos] == ' ' || source[pos] == '\t' || source[pos] == '\r'))
			++pos;
		return pos;
	}

	inline size_t ReadIdent(const std::string& source, size_t pos, std::string* out)
	{
		if (pos >= source.size() || !IsIdentStart(source[pos]))
			return std::string::npos;

		const size_t begin = pos;
		while (pos < source.size() && IsIdentChar(source[pos]))
			++pos;

		if (out != nullptr)
			out->assign(source, begin, pos - begin);
		return pos;
	}

	/* 指定位置的词是否正好是 word */
	inline bool IsWordAt(const std::string& source, size_t pos, const char* word)
	{
		const size_t length = std::strlen(word);
		if (source.compare(pos, length, word) != 0)
			return false;
		return pos + length >= source.size() || !IsIdentChar(source[pos + length]);
	}

	/* 按词找关键字（两侧不能接标识符字符） */
	inline size_t FindWord(const std::string& source, size_t from, const char* word)
	{
		const size_t length = std::strlen(word);
		while (from < source.size())
		{
			const size_t pos = source.find(word, from);
			if (pos == std::string::npos)
				return std::string::npos;

			if ((pos == 0 || !IsIdentChar(source[pos - 1]))
				&& (pos + length >= source.size() || !IsIdentChar(source[pos + length])))
				return pos;

			from = pos + 1;
		}
		return std::string::npos;
	}

	inline void TrimText(std::string& text)
	{
		const size_t first = text.find_first_not_of(" \t\r\n\v\f");
		if (first == std::string::npos)
		{
			text.clear();
			return;
		}
		const size_t last = text.find_last_not_of(" \t\r\n\v\f");
		text = text.substr(first, last - first + 1);
	}

	/* 紧挨在 uniform 前面的 layout(...)（中间只允许空白） */
	inline bool ReadLayoutBefore(const std::string& source, size_t pos, std::string& qualifiers)
	{
		size_t end = pos;
		while (end > 0 && IsSpace(source[end - 1]))
			--end;

		if (end == 0 || source[end - 1] != ')')
			return false;

		const size_t close = end - 1;
		const size_t open = source.rfind('(', close);
		if (open == std::string::npos)
			return false;

		size_t keyword_end = open;
		while (keyword_end > 0 && IsSpace(source[keyword_end - 1]))
			--keyword_end;

		if (keyword_end < 6 || source.compare(keyword_end - 6, 6, "layout") != 0)
			return false;
		if (keyword_end > 6 && IsIdentChar(source[keyword_end - 7]))
			return false;

		qualifiers.assign(source, open + 1, close - open - 1);
		return true;
	}

	/* `binding = N` 且整段只有它（对应 layout(binding = N) 的精确形态） */
	inline bool ParseOnlyBinding(const std::string& qualifiers, uint32_t& binding)
	{
		size_t pos = SkipSpaces(qualifiers, 0);
		if (qualifiers.compare(pos, 7, "binding") != 0)
			return false;
		pos += 7;

		pos = SkipSpaces(qualifiers, pos);
		if (pos >= qualifiers.size() || qualifiers[pos] != '=')
			return false;
		pos = SkipSpaces(qualifiers, pos + 1);

		const size_t digits_begin = pos;
		while (pos < qualifiers.size() && qualifiers[pos] >= '0' && qualifiers[pos] <= '9')
			++pos;
		if (pos == digits_begin)
			return false;

		binding = static_cast<uint32_t>(std::stoul(qualifiers.substr(digits_begin, pos - digits_begin)));
		return SkipSpaces(qualifiers, pos) == qualifiers.size();
	}

	/* `binding = N` 出现在限定符任意位置（uniform 块用） */
	inline bool ParseAnyBinding(const std::string& qualifiers, uint32_t& binding)
	{
		const size_t at = FindWord(qualifiers, 0, "binding");
		if (at == std::string::npos)
			return false;

		size_t pos = SkipSpaces(qualifiers, at + 7);
		if (pos >= qualifiers.size() || qualifiers[pos] != '=')
			return false;
		pos = SkipSpaces(qualifiers, pos + 1);

		const size_t digits_begin = pos;
		while (pos < qualifiers.size() && qualifiers[pos] >= '0' && qualifiers[pos] <= '9')
			++pos;
		if (pos == digits_begin)
			return false;

		binding = static_cast<uint32_t>(std::stoul(qualifiers.substr(digits_begin, pos - digits_begin)));
		return true;
	}

	inline bool IsTextureLikeType(const std::string& type)
	{
		return type.rfind("sampler", 0) == 0 || type.rfind("image", 0) == 0
			|| type.rfind("texture", 0) == 0 || type.rfind("isampler", 0) == 0
			|| type.rfind("usampler", 0) == 0;
	}

	/* 一条 uniform 声明（贴图 / 标量 / 块） */
	struct ScannedDecl
	{
		enum class Kind : uint8_t { Texture, Scalar, Block };

		Kind DeclKind{ Kind::Scalar };
		std::string TypeName;
		std::string Name;
		std::string Qualifiers{};
		bool HasLayout{ false };
		bool HasArray{ false };
		bool HasInit{ false };
		std::string InitExpr{};
		std::string Comment{};
	};

	inline bool ParseUniformDeclAt(const std::string& source, size_t uniform_pos, ScannedDecl& decl)
	{
		decl.HasLayout = ReadLayoutBefore(source, uniform_pos, decl.Qualifiers);

		const size_t keyword_end = uniform_pos + std::strlen("uniform");
		size_t pos = SkipSpaces(source, keyword_end);
		if (pos == keyword_end)
			return false; /* uniform 后面要有空白 */

		for (const char* precision : { "highp", "mediump", "lowp" })
		{
			if (!IsWordAt(source, pos, precision))
				continue;

			const size_t after = pos + std::strlen(precision);
			const size_t next = SkipSpaces(source, after);
			if (next == after)
				return false; /* 精度限定符后面要有空白 */

			pos = next;
			break;
		}

		pos = ReadIdent(source, pos, &decl.TypeName);
		if (pos == std::string::npos)
			return false;

		pos = SkipSpaces(source, pos);

		/* uniform Block { ... }：块声明没有变量名 */
		if (pos < source.size() && source[pos] == '{')
		{
			decl.DeclKind = ScannedDecl::Kind::Block;
			return true;
		}

		pos = ReadIdent(source, pos, &decl.Name);
		if (pos == std::string::npos)
			return false;

		pos = SkipSpaces(source, pos);
		if (pos < source.size() && source[pos] == '[')
		{
			const size_t close = source.find(']', pos + 1);
			if (close == std::string::npos)
				return false;

			decl.HasArray = true;
			pos = SkipSpaces(source, close + 1);
		}

		if (pos < source.size() && source[pos] == '=')
		{
			const size_t semi = source.find(';', pos + 1);
			if (semi == std::string::npos)
				return false;

			decl.HasInit = true;
			decl.InitExpr.assign(source, pos + 1, semi - pos - 1);
			TrimText(decl.InitExpr);
			pos = semi + 1;
		}
		else if (pos < source.size() && source[pos] == ';')
		{
			++pos;
		}
		else
		{
			return false;
		}

		pos = SkipInlineSpaces(source, pos);
		if (pos + 1 < source.size() && source[pos] == '/' && source[pos + 1] == '/')
		{
			const size_t line_end = source.find('\n', pos);
			const size_t end = (line_end == std::string::npos) ? source.size() : line_end;
			decl.Comment.assign(source, pos + 2, end - pos - 2);
		}

		decl.DeclKind = IsTextureLikeType(decl.TypeName) ? ScannedDecl::Kind::Texture
														: ScannedDecl::Kind::Scalar;
		return true;
	}

	inline void ScanUniformDecls(const std::string& source, std::vector<ScannedDecl>& out)
	{
		size_t cursor = 0;
		while (true)
		{
			const size_t uniform_pos = FindWord(source, cursor, "uniform");
			if (uniform_pos == std::string::npos)
				return;

			cursor = uniform_pos + std::strlen("uniform");

			ScannedDecl decl;
			if (ParseUniformDeclAt(source, uniform_pos, decl))
				out.push_back(std::move(decl));
		}
	}
}
