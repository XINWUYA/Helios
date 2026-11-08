#pragma once
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "Helios/Renderer/RenderCommon.h"

namespace Helios
{
	/* 着色器源码里的行尾标注（@property / @group / @type / @range / @showif / @variant）。
	 * 纯文本解析，不依赖任何后端：反射与变体规范化共用这一份，别各写一套。 */
	namespace ShaderAnnotation
	{
		inline std::string Trim(const std::string& text)
		{
			size_t begin = 0;
			size_t end = text.size();
			while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
				++begin;
			while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
				--end;
			return text.substr(begin, end - begin);
		}

		/* 取指令的括号参数：`@range(0, 1)` -> "0, 1"；光秃秃的 `@property` 返回空串 */
		inline std::string DirectiveArg(const std::string& comment, const char* directive)
		{
			size_t pos = comment.find(directive);
			while (pos != std::string::npos)
			{
				const size_t after = pos + std::strlen(directive);
				if (after >= comment.size())
					return {};

				/* 紧跟空白 = 没有括号参数 */
				if (std::isspace(static_cast<unsigned char>(comment[after])))
					return {};

				if (comment[after] == '(')
				{
					const size_t close = comment.find(')', after);
					if (close == std::string::npos)
						return {};
					return Trim(comment.substr(after + 1, close - after - 1));
				}

				pos = comment.find(directive, after);
			}
			return {};
		}

		inline std::vector<std::string> SplitArgs(const std::string& text)
		{
			std::vector<std::string> args;
			size_t begin = 0;
			while (true)
			{
				const size_t comma = text.find(',', begin);
				args.push_back(Trim(text.substr(begin,
					comma == std::string::npos ? std::string::npos : comma - begin)));
				if (comma == std::string::npos)
					break;
				begin = comma + 1;
			}
			return args;
		}

		/* 取一行里 uniform 声明的参数名；不是 uniform 声明返回空串 */
		inline std::string UniformNameInLine(const std::string& line)
		{
			const size_t uniform_pos = line.find("uniform");
			if (uniform_pos == std::string::npos)
				return {};

			const size_t semicolon = line.find(';', uniform_pos);
			if (semicolon == std::string::npos)
				return {};

			std::string decl = line.substr(uniform_pos + 7, semicolon - uniform_pos - 7);
			if (const size_t assign = decl.find('='); assign != std::string::npos)
				decl.erase(assign);
			if (const size_t bracket = decl.find('['); bracket != std::string::npos)
				decl.erase(bracket);

			/* 类型之后最后一个标识符就是参数名 */
			const size_t name_end = decl.find_last_not_of(" \t\r\n");
			if (name_end == std::string::npos)
				return {};
			const size_t name_start = decl.find_last_of(" \t\r\n", name_end);
			return (name_start == std::string::npos)
				? decl.substr(0, name_end + 1)
				: decl.substr(name_start + 1, name_end - name_start);
		}

		/* 声明行里 uniform 的类型；认不出返回 false。只认材质参数能承载的几种 */
		inline bool UniformTypeInLine(const std::string& line, ParamType& out)
		{
			const size_t uniform_pos = line.find("uniform");
			if (uniform_pos == std::string::npos)
				return false;

			size_t pos = uniform_pos + 7;
			while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos])))
				++pos;

			const size_t begin = pos;
			while (pos < line.size() && (std::isalnum(static_cast<unsigned char>(line[pos])) || line[pos] == '_'))
				++pos;
			if (pos == begin)
				return false;

			const std::string type = line.substr(begin, pos - begin);
			if (type == "int" || type == "uint")
				out = ParamType::Int;
			else if (type == "float")
				out = ParamType::Float;
			else if (type == "vec2")
				out = ParamType::Vec2;
			else if (type == "vec3")
				out = ParamType::Vec3;
			else if (type == "vec4")
				out = ParamType::Vec4;
			else if (type == "mat4")
				out = ParamType::Mat4;
			else if (type.rfind("sampler", 0) == 0)
				out = ParamType::Texture;
			else
				return false;
			return true;
		}

		inline bool KeywordEquals(const std::string& text, const char* keyword)
		{
			const size_t length = std::strlen(keyword);
			if (text.size() != length)
				return false;
			for (size_t i = 0; i < length; ++i)
			{
				if (std::tolower(static_cast<unsigned char>(text[i])) != keyword[i])
					return false;
			}
			return true;
		}

		/* 把 @variant 的第二个参数转成取值来源，认不出返回 false */
		inline bool VariantSourceFromKeyword(const std::string& text, ShaderVariantSource& source)
		{
			if (KeywordEquals(text, "external"))
			{
				source = ShaderVariantSource::External;
				return true;
			}
			if (KeywordEquals(text, "view"))
			{
				source = ShaderVariantSource::View;
				return true;
			}
			if (KeywordEquals(text, "global"))
			{
				source = ShaderVariantSource::Global;
				return true;
			}
			return false;
		}

		/* 变体维度：逐行找 @variant(NAME) / @variant(NAME, 来源)。行尾标注绑到该行声明的参数，
		 * 单独一行或写了来源关键字的就是不绑参数的那种。同名先到者定。
		 * unknown_sources 非空时收集认不出的来源关键字（拼成 "维度名:关键字"），加载时报错用。 */
		inline std::vector<ShaderVariantSpec> ParseVariantSpecs(const std::string& source,
			std::vector<std::string>* unknown_sources = nullptr)
		{
			std::vector<ShaderVariantSpec> specs;
			if (source.empty())
				return specs;

			const auto add = [&specs](ShaderVariantSpec spec)
			{
				for (const ShaderVariantSpec& existing : specs)
				{
					if (existing.Name == spec.Name)
						return;
				}
				specs.push_back(std::move(spec));
			};

			size_t line_begin = 0;
			while (line_begin <= source.size())
			{
				const size_t line_end = source.find('\n', line_begin);
				const std::string line = source.substr(line_begin,
					line_end == std::string::npos ? std::string::npos : line_end - line_begin);

				if (line.find("@variant") != std::string::npos)
				{
					const std::vector<std::string> args = SplitArgs(DirectiveArg(line, "@variant"));
					if (!args.empty() && !args[0].empty())
					{
						ShaderVariantSpec spec;
						spec.Name = args[0];

						if (args.size() > 1)
						{
							/* 写了来源关键字就不再绑参数；拼错的按外部维度处理，由调用方报 */
							if (!VariantSourceFromKeyword(args[1], spec.Source))
							{
								spec.Source = ShaderVariantSource::External;
								if (unknown_sources != nullptr)
									unknown_sources->push_back(args[0] + ":" + args[1]);
							}
						}
						else
						{
							spec.ParamName = UniformNameInLine(line);
							spec.ParamKindKnown = UniformTypeInLine(line, spec.ParamKind);
							spec.Source = spec.ParamName.empty()
								? ShaderVariantSource::External
								: ShaderVariantSource::Param;
						}

						add(std::move(spec));
					}
				}

				if (line_end == std::string::npos)
					break;
				line_begin = line_end + 1;
			}
			return specs;
		}

		/* 按声明把请求补成全集：声明过没请求的补 0，请求了没声明的丢进 unknown_dims。
		 * 这样"全默认"的空表与显式写 0 编出同一份源码；拼错的维度名也不会静默多编一份。 */
		inline ShaderVariantDefines NormalizeVariantDefines(const std::vector<ShaderVariantSpec>& specs,
			const ShaderVariantDefines& requested, std::vector<std::string>* unknown_dims = nullptr)
		{
			ShaderVariantDefines normalized;
			for (const ShaderVariantSpec& spec : specs)
			{
				const auto it = requested.find(spec.Name);
				normalized[spec.Name] = (it == requested.end()) ? 0 : it->second;
			}

			if (unknown_dims != nullptr)
			{
				for (const auto& [name, value] : requested)
				{
					(void)value;
					if (normalized.find(name) == normalized.end())
						unknown_dims->push_back(name);
				}
			}
			return normalized;
		}
	}
}
