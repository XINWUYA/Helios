#pragma once
#include <cstddef>
#include <cstdint>

namespace Helios
{
	namespace ProfileHash
	{
		constexpr uint64_t Hash(const char* text)
		{
			uint64_t hash = 1469598103934665603ull;
			for (size_t index = 0; text[index] != '\0'; ++index)
			{
				hash ^= static_cast<uint64_t>(static_cast<unsigned char>(text[index]));
				hash *= 1099511628211ull;
			}
			return hash;
		}
	}

	namespace ProfilerUtils
	{
		template <size_t N>
		struct ChangeResult
		{
			char Data[N];
		};

		/* expr 在 index 处是否是 token（编译期前缀判断） */
		template <size_t N>
		constexpr bool HasTokenAt(const char(&expr)[N], size_t index, const char* token)
		{
			for (size_t k = 0; token[k] != '\0'; ++k)
			{
				if (index + k >= N || expr[index + k] != token[k])
					return false;
			}
			return true;
		}

		/* 把编译器函数签名裁成「类::函数名」：剥掉返回类型与 virtual/static 等修饰符、
		 * 去掉参数表，顺带剥 MSVC 的 "__cdecl " 与 clang 的 "(anonymous namespace)"、
		 * 双引号换单引号。无参数表的自定义标签（"Update Layers" 等）原样保留。 */
		template <size_t N>
		constexpr auto CleanupFunctionLabel(const char(&expr)[N])
		{
			ChangeResult<N> result = {};

			/* 一、逐字符清理：剥两个固定标记、引号转单引号 */
			size_t out = 0;
			for (size_t i = 0; i < N && expr[i] != '\0';)
			{
				if (HasTokenAt(expr, i, "__cdecl "))
				{
					i += 8;
					continue;
				}
				if (HasTokenAt(expr, i, "(anonymous namespace)"))
				{
					i += 21;
					continue;
				}
				result.Data[out++] = expr[i] == '"' ? '\'' : expr[i];
				++i;
			}

			/* 二、取第一个 '(' 之前、类名起点开始的一段（无 '(' 视为自定义标签，原样返回） */
			size_t paren = out;
			for (size_t k = 0; k < out; ++k)
			{
				if (result.Data[k] == '(')
				{
					paren = k;
					break;
				}
			}
			if (paren == out)
				return result;

			/* 从 '(' 往左找名字起点：跳过模板尖括号内的空格（返回类型里的 "int, int>" 等） */
			size_t start = 0;
			size_t depth = 0;
			for (size_t k = paren; k-- > 0;)
			{
				const char ch = result.Data[k];
				if (ch == '>')
					++depth;
				else if (ch == '<' && depth > 0)
					--depth;
				else if (ch == ' ' && depth == 0)
				{
					start = k + 1;
					break;
				}
			}
			/* 匿名命名空间清理后可能带前导 "::"：跳过 */
			if (start + 1 < paren && result.Data[start] == ':' && result.Data[start + 1] == ':')
				start += 2;

			size_t write = 0;
			for (size_t k = start; k < paren; ++k)
				result.Data[write++] = result.Data[k];
			result.Data[write] = '\0';
			return result;
		}
	}
}
