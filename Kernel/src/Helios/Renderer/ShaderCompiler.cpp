#include "Pch.h"
#include "ShaderCompiler.h"

#include "Helios/Common/Assert.h"
#include "Helios/Common/Utils.h"
#include "Helios/Renderer/ShaderAnnotation.h"
#include "Helios/Scene/SceneCommon.h"

#include <xxhash.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <optional>
#include <regex>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>
#include <spirv_cross.hpp>
#include <spirv_msl.hpp>

/* MSL 的绑定槽位约定。该头文件不依赖任何 Metal/Objective-C 类型，
 * 且 GLSL 中的裸 uniform 会被 glslang 聚合为“默认 uniform 块”，
 * 需要在平台无关的 CompileGLSLToSPIRV 阶段就为它指定专用 binding 槽位。 */
#include "GraphicsAPI/Metal/MetalBindings.h"

namespace Helios
{
    namespace
    {
        /* 自动分配槽位的几类声明 */
        enum class SlotKind
        {
            LooseUniform, /* 裸 uniform（非 opaque、非块）的 location */
            Sampler,      /* sampler 的 binding */
            Input,        /* in 的 location */
            Output        /* out 的 location */
        };

        /* 单行的槽位声明 */
        struct SlotDecl
        {
            SlotKind Kind = SlotKind::LooseUniform;
            std::string Indent;
            std::string Qualifiers;  /* layout(...) 里去掉 location / binding 后剩下的部分 */
            std::string Declaration; /* layout(...) 之后的原文 */
            std::string Type;
            std::string Name;
            uint32_t ArraySize = 1;
        };

        /* 结构体名 -> 成员（类型, 数组长度） */
        using StructTable = std::unordered_map<std::string, std::vector<std::pair<std::string, uint32_t>>>;

        /* ---- 逐行声明扫描 ----
         * 原来这里两条大正则每行各跑一次 regex_match，libc++ 每次都要重建状态机，
         * 一个大着色器几千行就是几百毫秒。下面改成手写扫描，判定规则与原来等价。 */
        [[nodiscard]] bool IsDeclSpace(char c)
        {
            return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
        }

        [[nodiscard]] bool IsIdentStart(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        }

        [[nodiscard]] bool IsIdentChar(char c)
        {
            return IsIdentStart(c) || (c >= '0' && c <= '9');
        }

        [[nodiscard]] size_t SkipDeclSpace(const std::string& line, size_t pos)
        {
            while (pos < line.size() && IsDeclSpace(line[pos]))
                ++pos;
            return pos;
        }

        /* 读一个标识符，返回结束位置；不是标识符返回 npos */
        [[nodiscard]] size_t ReadIdent(const std::string& line, size_t pos, std::string* out)
        {
            if (pos >= line.size() || !IsIdentStart(line[pos]))
                return std::string::npos;

            const size_t begin = pos;
            while (pos < line.size() && IsIdentChar(line[pos]))
                ++pos;

            if (out != nullptr)
                out->assign(line, begin, pos - begin);
            return pos;
        }

        /* 该位置是不是独立的关键字（后面不能再接标识符字符） */
        [[nodiscard]] bool IsKeywordAt(const std::string& line, size_t pos, const char* keyword)
        {
            const size_t length = std::strlen(keyword);
            if (line.compare(pos, length, keyword) != 0)
                return false;

            const size_t next = pos + length;
            return next >= line.size() || !IsIdentChar(line[next]);
        }

        /* 声明前缀：缩进 + 可选的 layout(...)，返回正文起点 */
        struct DeclPrefix
        {
            std::string Indent;
            std::string Qualifiers;
            size_t BodyBegin = 0;
        };

        [[nodiscard]] bool ParseDeclPrefix(const std::string& line, DeclPrefix& prefix)
        {
            const size_t indent_end = SkipDeclSpace(line, 0);
            prefix.Indent.assign(line, 0, indent_end);

            size_t pos = indent_end;
            if (!IsKeywordAt(line, pos, "layout"))
            {
                prefix.BodyBegin = pos;
                return true;
            }

            pos += std::strlen("layout");
            while (pos < line.size() && IsDeclSpace(line[pos]))
                ++pos;
            if (pos >= line.size() || line[pos] != '(')
                return false;

            const size_t open = pos + 1;
            const size_t close = line.find(')', open);
            if (close == std::string::npos)
                return false;

            prefix.Qualifiers.assign(line, open, close - open);
            prefix.BodyBegin = SkipDeclSpace(line, close + 1);
            return true;
        }

        /* `uniform <precision>? <type> <name> [<size>]?` 后面必须紧跟 = ; 或 , */
        [[nodiscard]] bool ParseUniformDecl(const std::string& line, size_t pos, SlotDecl& decl,
            std::string& array_text, bool& has_array, char& tail_first)
        {
            if (!IsKeywordAt(line, pos, "uniform"))
                return false;

            pos += std::strlen("uniform");
            size_t next = SkipDeclSpace(line, pos);
            if (next == pos)
                return false; /* uniform 后面必须有空白 */
            pos = next;

            if (IsKeywordAt(line, pos, "highp") || IsKeywordAt(line, pos, "mediump")
                || IsKeywordAt(line, pos, "lowp"))
            {
                if (IsKeywordAt(line, pos, "highp"))
                    pos += std::strlen("highp");
                else if (IsKeywordAt(line, pos, "mediump"))
                    pos += std::strlen("mediump");
                else
                    pos += std::strlen("lowp");

                next = SkipDeclSpace(line, pos);
                if (next == pos)
                    return false;
                pos = next;
            }

            pos = ReadIdent(line, pos, &decl.Type);
            if (pos == std::string::npos)
                return false;

            next = SkipDeclSpace(line, pos);
            if (next == pos)
                return false; /* 类型与名字之间必须有空白 */
            pos = ReadIdent(line, next, &decl.Name);
            if (pos == std::string::npos)
                return false;

            pos = SkipDeclSpace(line, pos);
            has_array = false;
            array_text.clear();

            if (pos < line.size() && line[pos] == '[')
            {
                const size_t close = line.find(']', pos + 1);
                if (close == std::string::npos)
                    return false;

                array_text = line.substr(pos + 1, close - pos - 1);
                const size_t first = array_text.find_first_not_of(" \t\r\v\f");
                const size_t last = array_text.find_last_not_of(" \t\r\v\f");
                array_text = (first == std::string::npos)
                    ? std::string{}
                    : array_text.substr(first, last - first + 1);
                has_array = true;
                pos = SkipDeclSpace(line, close + 1);
            }

            if (pos >= line.size())
                return false;

            tail_first = line[pos];
            return tail_first == '=' || tail_first == ';' || tail_first == ',';
        }

        /* `<插值限定符>* in|out <限定符>* <type> <name> [<size>]? ;` */
        [[nodiscard]] bool ParseVaryingDecl(const std::string& line, size_t pos, SlotDecl& decl,
            std::string& array_text, bool& has_array)
        {
            static const char* const kPrefixQualifiers[] = {
                "flat", "noperspective", "smooth", "centroid", "sample", "invariant" };
            static const char* const kAfterQualifiers[] = {
                "flat", "noperspective", "smooth", "centroid", "sample", "highp", "mediump", "lowp" };

            const auto consume_qualifier = [&line, &pos](const char* const* names, size_t count)
            {
                for (size_t index = 0; index < count; ++index)
                {
                    if (!IsKeywordAt(line, pos, names[index]))
                        continue;

                    /* 限定符后面必须跟空白，否则这一项不算（位置不前进） */
                    const size_t after = pos + std::strlen(names[index]);
                    const size_t next = SkipDeclSpace(line, after);
                    if (next == after)
                        return false;

                    pos = next;
                    return true;
                }
                return false;
            };

            while (consume_qualifier(kPrefixQualifiers, std::size(kPrefixQualifiers)))
            {
            }

            if (IsKeywordAt(line, pos, "in"))
            {
                decl.Kind = SlotKind::Input;
                pos += 2;
            }
            else if (IsKeywordAt(line, pos, "out"))
            {
                decl.Kind = SlotKind::Output;
                pos += 3;
            }
            else
            {
                return false;
            }

            size_t next = SkipDeclSpace(line, pos);
            if (next == pos)
                return false;
            pos = next;

            while (consume_qualifier(kAfterQualifiers, std::size(kAfterQualifiers)))
            {
            }

            pos = ReadIdent(line, pos, &decl.Type);
            if (pos == std::string::npos)
                return false;

            next = SkipDeclSpace(line, pos);
            if (next == pos)
                return false;
            pos = ReadIdent(line, next, &decl.Name);
            if (pos == std::string::npos)
                return false;

            pos = SkipDeclSpace(line, pos);
            has_array = false;
            array_text.clear();

            if (pos < line.size() && line[pos] == '[')
            {
                const size_t close = line.find(']', pos + 1);
                if (close == std::string::npos)
                    return false;

                array_text = line.substr(pos + 1, close - pos - 1);
                const size_t first = array_text.find_first_not_of(" \t\r\v\f");
                const size_t last = array_text.find_last_not_of(" \t\r\v\f");
                array_text = (first == std::string::npos)
                    ? std::string{}
                    : array_text.substr(first, last - first + 1);
                has_array = true;
                pos = SkipDeclSpace(line, close + 1);
            }

            return pos < line.size() && line[pos] == ';';
        }

        [[nodiscard]] bool IsBasicGlslType(const std::string& type)
        {
            if (type == "bool" || type == "int" || type == "uint" || type == "float" || type == "double")
                return true;

            if (type.size() == 4 && type[3] >= '2' && type[3] <= '4')
            {
                const std::string head = type.substr(0, 3);
                if (head == "vec" || head == "ivec" || head == "uvec" || head == "bvec")
                    return true;
            }

            /* matN / dmatN，可带 xM 后缀 */
            size_t pos = (!type.empty() && type[0] == 'd') ? 1 : 0;
            if (type.compare(pos, 3, "mat") != 0)
                return false;

            pos += 3;
            if (pos >= type.size() || type[pos] < '2' || type[pos] > '4')
                return false;
            ++pos;

            if (pos == type.size())
                return true;

            return type[pos] == 'x' && pos + 2 == type.size()
                && type[pos + 1] >= '2' && type[pos + 1] <= '4';
        }

        [[nodiscard]] bool IsAllDigits(const std::string& text)
        {
            if (text.empty())
                return false;

            for (const char c : text)
            {
                if (c < '0' || c > '9')
                    return false;
            }
            return true;
        }

        /* 限定符项是不是 location / binding 赋值（这两项由分配器重写，其余原样保留） */
        [[nodiscard]] bool IsSlotQualifier(const std::string& item)
        {
            size_t pos = 0;
            if (item.compare(0, 8, "location") == 0)
                pos = 8;
            else if (item.compare(0, 7, "binding") == 0)
                pos = 7;
            else
                return false;

            while (pos < item.size() && IsDeclSpace(item[pos]))
                ++pos;
            return pos < item.size() && item[pos] == '=';
        }

        bool IsSamplerType(const std::string& type)
        {
            return type.rfind("sampler", 0) == 0 || type.rfind("isampler", 0) == 0 || type.rfind("usampler", 0) == 0;
        }

        bool IsOpaqueUniformType(const std::string& type)
        {
            static const char* const kPrefixes[] = { "sampler", "isampler", "usampler", "image", "iimage",
                "uimage", "texture", "itexture", "utexture", "subpassInput", "atomic_uint" };
            for (const char* prefix : kPrefixes)
            {
                if (type.rfind(prefix, 0) == 0)
                    return true;
            }
            return false;
        }

        /* 逐行扫描时维护块注释状态，返回这一行结束时是否仍在块注释里 */
        bool UpdateBlockCommentState(const std::string& line, bool in_comment)
        {
            size_t pos = 0;
            while (pos < line.size())
            {
                if (in_comment)
                {
                    const size_t close = line.find("*/", pos);
                    if (close == std::string::npos)
                        return true;
                    in_comment = false;
                    pos = close + 2;
                }
                else
                {
                    const size_t open = line.find("/*", pos);
                    const size_t line_comment = line.find("//", pos);
                    if (open == std::string::npos || (line_comment != std::string::npos && line_comment < open))
                        return false;
                    in_comment = true;
                    pos = open + 2;
                }
            }
            return in_comment;
        }

        /* 注释换成空格（换行保留），给结构体解析用 */
        std::string StripComments(const std::string& source)
        {
            std::string result = source;
            size_t pos = 0;
            while (pos < result.size())
            {
                const bool line_comment = result.compare(pos, 2, "//") == 0;
                const bool block_comment = result.compare(pos, 2, "/*") == 0;
                if (!line_comment && !block_comment)
                {
                    ++pos;
                    continue;
                }

                const size_t end = line_comment ? result.find('\n', pos) : result.find("*/", pos + 2);
                const size_t stop = end == std::string::npos ? result.size() : (line_comment ? end : end + 2);
                for (size_t i = pos; i < stop; ++i)
                {
                    if (result[i] != '\n')
                        result[i] = ' ';
                }
                pos = stop;
            }
            return result;
        }

        StructTable CollectStructs(const std::string& source)
        {
            static const std::regex struct_regex(R"(struct\s+([A-Za-z_]\w*)\s*\{([^}]*)\})");
            static const std::regex member_regex(
                R"(^\s*(?:(?:highp|mediump|lowp|flat|noperspective|smooth|centroid)\s+)*([A-Za-z_]\w*)\s+([^;]+)$)");
            static const std::regex name_regex(R"(^\s*[A-Za-z_]\w*\s*(?:\[\s*(\d+)\s*\])?\s*$)");

            StructTable structs;
            const std::string text = StripComments(source);
            for (auto it = std::sregex_iterator(text.begin(), text.end(), struct_regex); it != std::sregex_iterator(); ++it)
            {
                auto& members = structs[it->str(1)];
                std::stringstream body(it->str(2));
                std::string statement;
                while (std::getline(body, statement, ';'))
                {
                    std::smatch member;
                    if (!std::regex_match(statement, member, member_regex))
                        continue;

                    std::stringstream names(member.str(2));
                    std::string name;
                    while (std::getline(names, name, ','))
                    {
                        std::smatch name_match;
                        if (std::regex_match(name, name_match, name_regex))
                            members.emplace_back(member.str(1),
                                name_match[1].matched ? static_cast<uint32_t>(std::stoul(name_match.str(1))) : 1u);
                    }
                }
            }
            return structs;
        }

        /* in / out 占用的 location 个数：矩阵按列算，dvec3 / dvec4 占两个，结构体按成员累加 */
        bool CountVaryingLocations(const std::string& type, const StructTable& structs, uint32_t& count, int depth = 0)
        {
            static const std::regex single_regex(R"((?:bool|int|uint|float|double|[biu]?vec[234]|dvec2))");
            static const std::regex wide_regex(R"((?:dvec[34]))");
            static const std::regex matrix_regex(R"((d?)mat([234])(?:x([234]))?)");

            std::smatch match;
            if (std::regex_match(type, single_regex))
            {
                count = 1;
                return true;
            }
            if (std::regex_match(type, wide_regex))
            {
                count = 2;
                return true;
            }
            if (std::regex_match(type, match, matrix_regex))
            {
                const uint32_t columns = static_cast<uint32_t>(std::stoul(match.str(2)));
                const uint32_t rows = match[3].matched ? static_cast<uint32_t>(std::stoul(match.str(3))) : columns;
                count = columns * (match.str(1) == "d" && rows >= 3 ? 2u : 1u);
                return true;
            }

            const auto it = structs.find(type);
            if (it == structs.end() || depth > 8)
                return false;

            count = 0;
            for (const auto& [member_type, array_size] : it->second)
            {
                uint32_t member_count = 0;
                if (!CountVaryingLocations(member_type, structs, member_count, depth + 1))
                    return false;
                count += member_count * array_size;
            }
            return true;
        }

        /* 逐行找槽位声明，on_decl 返回替换后的整行（nullopt 原样保留），结果写进 output */
        bool RewriteSlotDecls(const std::string& source,
            const std::function<std::optional<std::string>(const SlotDecl&)>& on_decl,
            std::string& output, std::string& error_msg)
        {
            output.clear();
            output.reserve(source.size());

            bool in_comment = false;
            size_t line_begin = 0;
            while (line_begin < source.size())
            {
                size_t line_end = source.find('\n', line_begin);
                if (line_end == std::string::npos)
                    line_end = source.size();

                std::string line = source.substr(line_begin, line_end - line_begin);
                const bool commented = in_comment;
                in_comment = UpdateBlockCommentState(line, in_comment);

                const bool has_cr = !line.empty() && line.back() == '\r';
                const std::string body = has_cr ? line.substr(0, line.size() - 1) : line;

                SlotDecl decl;
                bool found = false;
                std::string qualifiers;
                std::string array_text;
                bool has_array = false;
                DeclPrefix prefix;
                if (!commented && body.find("uniform") != std::string::npos && ParseDeclPrefix(body, prefix))
                {
                    char tail_first = '\0';
                    if (ParseUniformDecl(body, prefix.BodyBegin, decl, array_text, has_array, tail_first))
                    {
                        const std::string& type = decl.Type;
                        if (IsSamplerType(type))
                        {
                            decl.Kind = SlotKind::Sampler;
                            found = true;
                        }
                        else if (!IsOpaqueUniformType(type))
                        {
                            if (tail_first == ',')
                            {
                                error_msg = "uniform '" + decl.Name + "': declare one uniform per line";
                                return false;
                            }
                            if (!IsBasicGlslType(type))
                            {
                                error_msg = "uniform '" + decl.Name + "': type '" + type +
                                    "' is not a basic type, use a uniform block for structs";
                                return false;
                            }
                            decl.Kind = SlotKind::LooseUniform;
                            found = true;
                        }

                        if (found)
                        {
                            decl.Indent = prefix.Indent;
                            qualifiers = prefix.Qualifiers;
                            decl.Declaration = body.substr(prefix.BodyBegin);
                        }
                    }
                }
                else if (!commented && ParseDeclPrefix(body, prefix)
                    && ParseVaryingDecl(body, prefix.BodyBegin, decl, array_text, has_array))
                {
                    decl.Indent = prefix.Indent;
                    qualifiers = prefix.Qualifiers;
                    decl.Declaration = body.substr(prefix.BodyBegin);
                    found = true;
                }

                if (found)
                {
                    if (has_array)
                    {
                        if (!IsAllDigits(array_text))
                        {
                            error_msg = "'" + decl.Name + "': array size must be a number literal";
                            return false;
                        }
                        decl.ArraySize = static_cast<uint32_t>(std::stoul(array_text));
                    }

                    /* 保留 location / binding 以外的限定符 */
                    std::stringstream qualifier_items(qualifiers);
                    std::string item;
                    while (std::getline(qualifier_items, item, ','))
                    {
                        item.erase(0, item.find_first_not_of(" \t"));
                        item.erase(item.find_last_not_of(" \t") + 1);
                        if (item.empty() || IsSlotQualifier(item))
                            continue;
                        decl.Qualifiers += (decl.Qualifiers.empty() ? "" : ", ") + item;
                    }

                    if (const std::optional<std::string> replaced = on_decl(decl))
                        line = *replaced + (has_cr ? "\r" : "");
                }

                output += line;
                if (line_end < source.size())
                    output += '\n';
                line_begin = line_end + 1;
            }
            return true;
        }
    }

#ifdef PLATFORM_MACOS
    namespace
    {
        /* 收集 GLSL 里显式声明 binding 的 uniform block 的 binding 值。只有块形式（layout(...)
         * uniform Block）才算：裸 uniform 会被聚合成"默认 uniform 块"（没有显式 binding），靠这个
         * 跟显式块区分、映射到专用槽位。 */
        std::unordered_set<uint32_t> CollectExplicitUniformBlockBindings(const std::string& source)
        {
            /* 只匹配块声明 `layout(...) uniform <BlockName> {`；
             * `layout(...) uniform sampler2D x;` 这类非块声明不会命中。 */
            static const std::regex layout_regex(
                R"(layout\s*\(([^)]*)\)\s*uniform\s+\w+\s*\{)");
            static const std::regex binding_regex(R"(binding\s*=\s*(\d+))");

            std::unordered_set<uint32_t> bindings;
            for (auto it = std::sregex_iterator(source.begin(), source.end(), layout_regex);
                 it != std::sregex_iterator(); ++it)
            {
                const std::string qualifiers = (*it)[1].str();
                std::smatch binding_match;
                if (!std::regex_search(qualifiers, binding_match, binding_regex))
                    continue;

                bindings.insert(static_cast<uint32_t>(std::stoul(binding_match[1].str())));
            }
            return bindings;
        }
    }
#endif

    ShaderCompiler::ShaderStage ShaderCompiler::StringToShaderStage(const std::string& type)
    {
        if (type == "vertex")
            return ShaderStage::Vertex;
        if (type == "fragment" || type == "pixel")
            return ShaderStage::Fragment;
        if (type == "compute")
            return ShaderStage::Compute;

        CORE_LOG_ERROR("Unknown shader type: {}", type);
        ASSERT(false, "Unknown shader type!");
        return ShaderStage::Vertex;
    }

    const char* ShaderCompiler::ShaderStageToString(ShaderStage stage)
    {
        switch (stage)
        {
        case ShaderStage::Vertex: return "vert";
        case ShaderStage::Fragment: return "frag";
        case ShaderStage::Compute: return "comp";
        }
        return "unknown";
    }

    namespace
    {
        /* include 展开的递归上限，循环引用另外按 include 链检测 */
        constexpr size_t kMaxIncludeDepth = 64;

        /* 翻译规则版本：glslang 的编译规则、spirv-cross 的 MSL 版本与绑定槽位约定
         * 变化时，源码哈希不变但产物该失效。改这些规则请同时递增它。
         * 内容指纹也带上它，换代后热重载的"没变"判断不会被旧指纹挡住。 */
        constexpr const char* kTranslatorVersion = "helios-shader-translator-v6";

        /* glslang 的全局状态没有初始化调用可依赖，编译一律串行：
         * 首次加载在主线程、热重载在后台线程，两者可能撞上 */
        std::mutex& CompileMutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        /* 同一份着色器的同一个维度只报一次 —— 热重载一遍遍重编，不能跟着刷屏 */
        bool FirstReport(const std::string& filepath, const std::string& name)
        {
            static std::mutex mutex;
            static std::unordered_set<std::string> reported;

            std::lock_guard lock(mutex);
            return reported.insert(filepath + "#" + name).second;
        }

        /* 未声明维度在 Debug 下断点，编辑器里拼错名字会当场停下。自检要故意构造这种请求
         * 验证丢弃规则，跑回归时用 HELIOS_VARIANT_STRICT=0 关掉。 */
        bool VariantStrictEnabled()
        {
            static const bool enabled = []
            {
                const char* value = std::getenv("HELIOS_VARIANT_STRICT");
                return value == nullptr || std::strcmp(value, "0") != 0;
            }();
            return enabled;
        }

        /* 请求了源码没声明的变体维度：已经丢掉，这里报错。维度名写死在 pass 代码里，
         * 拼错就是 bug，不该静默。 */
        void ReportUnknownVariantDefines(const std::string& filepath, const std::vector<std::string>& names)
        {
            const std::string filename = std::filesystem::path(filepath).filename().string();
            for (const std::string& name : names)
            {
                if (!FirstReport(filepath, name))
                    continue;

                CORE_LOG_ERROR("[Shader] {} requests undeclared variant dimension '{}'", filename, name);
#ifdef HELIOS_DEBUG
                /* 热重载时"先删 shader 声明、后改 pass 调用"会先命中这里，两处要一起改 */
                if (VariantStrictEnabled())
                    Break();
#endif
            }
        }

        /* 取值只认 0..ShaderVariantMaxValue：开关量用 0 / 1，多档维度用中间值。
         * 越界只会多编一份跟谁都不重的源码，报一次让人回来看 */
        void ReportVariantValueOutOfRange(const std::string& filepath, const ShaderVariantDefines& requested)
        {
            const std::string filename = std::filesystem::path(filepath).filename().string();
            for (const auto& [name, value] : requested)
            {
                if ((value >= 0 && value <= ShaderVariantMaxValue) || !FirstReport(filepath, name + "#value"))
                    continue;
                CORE_LOG_WARN("[Shader] Variant '{}' of {} = {}, only 0..{} are supported",
                    name, filename, value, ShaderVariantMaxValue);
            }
        }

        /* 声明过的维度只能用 #if X 判断。补全默认值后 #define X 0 总会存在，
         * #ifdef X 与 defined(X) 恒为真，扫到就提示改写法。 */
        void ReportVariantConditionalMisuse(const std::string& filepath,
            const std::unordered_map<ShaderCompiler::ShaderStage, std::string>& stage_sources,
            const std::vector<ShaderVariantSpec>& specs)
        {
            const std::string filename = std::filesystem::path(filepath).filename().string();

            /* 前缀匹配要卡词边界，别让 USE 命中 USE_NORMAL */
            const auto contains_word = [](const std::string& source, const std::string& pattern)
            {
                size_t pos = source.find(pattern);
                while (pos != std::string::npos)
                {
                    const size_t next = pos + pattern.size();
                    if (next >= source.size())
                        return true;
                    const unsigned char ch = static_cast<unsigned char>(source[next]);
                    if (!std::isalnum(ch) && ch != '_')
                        return true;
                    pos = source.find(pattern, pos + 1);
                }
                return false;
            };

            for (const ShaderVariantSpec& spec : specs)
            {
                const std::string ifdef_pattern = "#ifdef " + spec.Name;
                const std::string ifndef_pattern = "#ifndef " + spec.Name;
                const std::string defined_pattern = "defined(" + spec.Name + ")";

                bool found = false;
                for (const auto& entry : stage_sources)
                {
                    const std::string& source = entry.second;
                    if (contains_word(source, ifdef_pattern) || contains_word(source, ifndef_pattern)
                        || source.find(defined_pattern) != std::string::npos)
                    {
                        found = true;
                        break;
                    }
                }

                if (found && FirstReport(filepath, spec.Name + "#ifdef"))
                {
                    CORE_LOG_WARN("[Shader] Variant '{}' of {} is tested with #ifdef/defined(), "
                        "which is always true; use #if {} instead", spec.Name, filename, spec.Name);
                }
            }
        }

        /* 材质参数维度的几条约定：类型、数量、前缀。
         * 加载时报一次，不阻断编译 —— 编辑器先能跑起来，改对了自然消失。
         * 与 external 维度可以混用：值由 Material::GetDrawShader 合成，见那里的叠加顺序。 */
        void ValidateParamVariants(const std::string& filepath, const std::vector<ShaderVariantSpec>& specs)
        {
            const std::string filename = std::filesystem::path(filepath).filename().string();

            size_t param_count = 0;
            for (const ShaderVariantSpec& spec : specs)
            {
                if (spec.Source == ShaderVariantSource::External)
                    continue;

                ++param_count;

                if (spec.Name.rfind("MTL_", 0) != 0 && FirstReport(filepath, spec.Name + "#prefix"))
                {
                    CORE_LOG_WARN("[Shader] Variant '{}' of {} is bound to a material parameter, "
                        "name it with an MTL_ prefix", spec.Name, filename);
                }

                const bool supported = spec.ParamKindKnown
                    && (spec.ParamKind == ParamType::Texture || spec.ParamKind == ParamType::Int
                        || spec.ParamKind == ParamType::Float);
                if (!supported && FirstReport(filepath, spec.Name + "#kind"))
                {
                    CORE_LOG_ERROR("[Shader] Variant '{}' of {} binds to a uniform type that cannot "
                        "drive a variant, only texture / int / float are supported", spec.Name, filename);
                }
            }

            if (param_count > 4 && FirstReport(filepath, "#limit"))
            {
                CORE_LOG_ERROR("[Shader] {} declares {} material variants, at most 4 are supported",
                    filename, param_count);
            }
        }


        /* 缓存目录的清理。产物按内容哈希命名，改一次源码就换一批文件名，所以不能按写入
         * 时间删 —— 那些每次启动都在用的稳定着色器反而是最老的。规则是"多久没被读过"：
         * 命中缓存时刷新修改时间（见 TryLoadCache），超过下面的天数才删。 */
        void TrimCache(const std::filesystem::path& dir)
        {
            /* 30 天没用过就删。UE 本地 DDC 的 UnusedFileAge 默认 15 天，编辑器项目改得慢，放宽一倍 */
            constexpr int64_t kUnusedDays = 30;
            /* 容量兜底：天数没到但文件堆太多时，仍然按最久没用删到八成 */
            constexpr uint64_t kMaxBytes = 256ull * 1024ull * 1024ull;
            constexpr size_t kMaxFiles = 4000;
            /* 扫描间隔：编译本身已经是几十毫秒级，扫目录也压到三十秒最多一次 */
            constexpr int64_t kTrimIntervalMs = 30000;

            static std::atomic<int64_t> last_trim_ms{ 0 };
            const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now_ms - last_trim_ms.load(std::memory_order_relaxed) < kTrimIntervalMs)
                return;
            last_trim_ms.store(now_ms, std::memory_order_relaxed);

            struct CacheEntry
            {
                std::filesystem::path Path;
                uint64_t Size{ 0 };
                std::filesystem::file_time_type Stamp{};
            };

            std::error_code error;
            std::vector<CacheEntry> files;
            uint64_t total_bytes = 0;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, error))
            {
                if (error)
                    break;
                if (!entry.is_regular_file(error) || error)
                {
                    error.clear();
                    continue;
                }

                CacheEntry item;
                item.Path = entry.path();
                item.Size = entry.file_size(error);
                item.Stamp = entry.last_write_time(error);
                error.clear();

                total_bytes += item.Size;
                files.push_back(std::move(item));
            }

            /* 超过天数没读过的先删掉 */
            const auto unused_edge = std::filesystem::file_time_type::clock::now()
                - std::chrono::hours(24 * kUnusedDays);
            size_t removed_unused = 0;
            for (const CacheEntry& item : files)
            {
                if (item.Stamp >= unused_edge)
                    continue;

                std::error_code remove_error;
                if (!std::filesystem::remove(item.Path, remove_error))
                    continue;

                ++removed_unused;
                total_bytes = (total_bytes > item.Size) ? total_bytes - item.Size : 0;
            }

            const size_t remaining = files.size() - removed_unused;
            size_t removed_capacity = 0;
            if (remaining > kMaxFiles || total_bytes > kMaxBytes)
            {
                std::sort(files.begin(), files.end(),
                    [](const CacheEntry& a, const CacheEntry& b) { return a.Stamp < b.Stamp; });

                /* 删到八成：留出余量，不然每次保存都会再触发一遍 */
                const size_t target_files = kMaxFiles * 8 / 10;
                const uint64_t target_bytes = kMaxBytes * 8 / 10;
                for (const CacheEntry& item : files)
                {
                    if (remaining - removed_capacity <= target_files && total_bytes <= target_bytes)
                        break;
                    if (item.Stamp < unused_edge)
                        continue; /* 上一轮删过的不重复处理 */

                    std::error_code remove_error;
                    if (!std::filesystem::remove(item.Path, remove_error))
                        continue;

                    ++removed_capacity;
                    total_bytes = (total_bytes > item.Size) ? total_bytes - item.Size : 0;
                }
            }

            const size_t removed = removed_unused + removed_capacity;
            if (removed > 0)
                CORE_LOG_INFO("[Shader] Cache trimmed: {} unused, {} over quota, {} files / {} MB left",
                    removed_unused, removed_capacity, files.size() - removed, total_bytes / (1024 * 1024));        }

        /* `#include "x"` 或 `#include <x>` 的路径；不是 include 行返回 false。
         * 原来每行跑两次 regex_search，改手写前缀匹配（规则一致：只认空格、路径非空）。 */
        [[nodiscard]] bool ParseIncludeLine(const std::string& line, std::string& out_path)
        {
            size_t pos = 0;
            while (pos < line.size() && line[pos] == ' ')
                ++pos;

            if (pos >= line.size() || line[pos] != '#')
                return false;
            ++pos;
            while (pos < line.size() && line[pos] == ' ')
                ++pos;

            if (line.compare(pos, 7, "include") != 0)
                return false;
            pos += 7;

            if (pos >= line.size() || line[pos] != ' ')
                return false; /* #include 后面至少要一个空格 */
            while (pos < line.size() && line[pos] == ' ')
                ++pos;

            if (pos >= line.size())
                return false;

            const char open = line[pos];
            if (open != '"' && open != '<')
                return false;

            const size_t end = line.find(open == '"' ? '"' : '>', pos + 1);
            if (end == std::string::npos || end == pos + 1)
                return false;

            out_path.assign(line, pos + 1, end - pos - 1);
            return true;
        }

        /* 收集依赖：只去重，不影响展开 */
        void AddDependency(std::vector<std::string>* dependencies, const std::string& path)
        {            if (dependencies == nullptr)
                return;

            for (const std::string& existing : *dependencies)
            {
                if (existing == path)
                    return;
            }
            dependencies->push_back(path);
        }
    }

    std::string ShaderCompiler::ReadFile(const std::string& filepath, std::vector<std::string>* dependencies)
    {
        IncludeContext context;
        context.Dependencies = dependencies;
        return ReadFile(filepath, context);
    }

    std::string ShaderCompiler::ReadFile(const std::string& filepath, IncludeContext& context)
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

        /* 同一文件被包含两次照样展开两次，只有出现在当前 include 链上才算循环 */
        const std::string key = std::filesystem::path(filepath).lexically_normal().generic_string();
        if (std::find(context.Stack.begin(), context.Stack.end(), key) != context.Stack.end())
        {
            CORE_LOG_ERROR("Shader include cycle at: {}", filepath);
            return {};
        }
        if (context.Stack.size() >= kMaxIncludeDepth)
        {
            CORE_LOG_ERROR("Shader include too deep ({} levels) at: {}", context.Stack.size(), filepath);
            return {};
        }

        AddDependency(context.Dependencies, filepath);

        std::ifstream in(filepath, std::ios::in | std::ios::binary);
        if (!in)
        {
            CORE_LOG_ERROR("Could not open shader file: {}", filepath);
            return {};
        }
        const std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        /* 原始字节只记第一次读到的那份，跟依赖表一一对应 */
        if (context.Contents != nullptr)
        {
            const auto recorded = std::find_if(context.Contents->begin(), context.Contents->end(),
                [&filepath](const auto& entry) { return entry.first == filepath; });
            if (recorded == context.Contents->end())
                context.Contents->emplace_back(filepath, raw);
        }

        context.Stack.push_back(key);

        std::string result;
        std::string line_str;
        std::istringstream lines(raw);
        while (std::getline(lines, line_str))
        {
            std::string include_file;
            if (line_str.find("#include") != line_str.npos && ParseIncludeLine(line_str, include_file))
            {
                std::string base_dir = ExtractFileBaseDir(filepath);
                result += ReadFile(base_dir + include_file, context) + '\n';
            }
            else
            {
                result += line_str + '\n';
            }
        }

        context.Stack.pop_back();
        return result;
    }

    void ShaderCompiler::PreProcessShaderSrc(const std::string& source,
        std::unordered_map<ShaderStage, std::string>& shader_sources)
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

        const char* type_token = "#type";
        const size_t type_token_length = strlen(type_token);
        size_t pos = source.find(type_token, 0);
        while (pos != std::string::npos)
        {
            const size_t eol = source.find_first_of("\r\n", pos);
            if (eol == std::string::npos)
            {
                /* 热重载会读到写了一半的文件，这里只报错不中断：
                 * 调用方看到空表就保留上一份能用的着色器 */
                CORE_LOG_ERROR("Shader syntax error: #type declaration has no newline.");
                shader_sources.clear();
                return;
            }

            const size_t begin = pos + type_token_length + 1;
            std::string type = source.substr(begin, eol - begin);
            ShaderStage stage = StringToShaderStage(type);

            const size_t next_line_pos = source.find_first_not_of("\r\n", eol);
            if (next_line_pos == std::string::npos)
            {
                CORE_LOG_ERROR("Shader syntax error: #type declaration has no shader code.");
                shader_sources.clear();
                return;
            }

            pos = source.find(type_token, next_line_pos);
            shader_sources[stage] = (pos == std::string::npos) ? source.substr(next_line_pos) : source.substr(next_line_pos, pos - next_line_pos);
        }
    }

    bool ShaderCompiler::AssignShaderSlots(std::unordered_map<ShaderStage, std::string>& stage_sources,
        bool write_uniform_locations, std::string& error_msg)
    {
        /* 同名共用一个槽位，按首次出现的顺序往后排 */
        struct SlotTable
        {
            struct Slot
            {
                std::string Type;
                uint32_t Count = 1;
                uint32_t Index = 0;
            };
            std::unordered_map<std::string, Slot> Slots;
            uint32_t Next = 0;

            bool Add(const std::string& name, const std::string& type, uint32_t count, std::string& error)
            {
                const auto [it, inserted] = Slots.emplace(name, Slot{ type, count, Next });
                if (inserted)
                {
                    Next += count;
                    return true;
                }
                if (it->second.Type == type && it->second.Count == count)
                    return true;
                error = "'" + name + "' is declared with different types: " + it->second.Type + " vs " + type;
                return false;
            }
        };

        /* Metal 每个阶段最多 16 个 sampler，GL 每阶段也只保证 16 个贴图单元 */
        static constexpr uint32_t kMaxSamplerBindings = 16;
        static constexpr ShaderStage kStageOrder[] = { ShaderStage::Vertex, ShaderStage::Fragment, ShaderStage::Compute };

        /* 顶点 out 和片元 in 按名字配对，其他 in / out（顶点输入、片元输出）保持手写 */
        const bool link_varyings = stage_sources.count(ShaderStage::Vertex) && stage_sources.count(ShaderStage::Fragment);
        const auto is_varying = [link_varyings](ShaderStage stage, const SlotDecl& decl)
        {
            return link_varyings && ((stage == ShaderStage::Vertex && decl.Kind == SlotKind::Output)
                || (stage == ShaderStage::Fragment && decl.Kind == SlotKind::Input));
        };
        const auto type_key = [](const SlotDecl& decl)
        {
            return decl.Type + (decl.ArraySize > 1 ? "[" + std::to_string(decl.ArraySize) + "]" : "");
        };

        SlotTable uniforms;
        SlotTable samplers;
        SlotTable varyings;
        std::vector<std::pair<std::string, std::pair<std::string, uint32_t>>> fragment_inputs;
        std::string scratch;

        /* 先把所有阶段的声明收齐再编号，出错时源码保持原样 */
        for (const ShaderStage stage : kStageOrder)
        {
            const auto source = stage_sources.find(stage);
            if (source == stage_sources.end())
                continue;

            const StructTable structs = CollectStructs(source->second);
            std::string conflict;
            const bool parsed = RewriteSlotDecls(source->second, [&](const SlotDecl& decl) -> std::optional<std::string>
            {
                if (!conflict.empty())
                    return std::nullopt;

                if (decl.Kind == SlotKind::LooseUniform)
                {
                    /* 一个 location 是一组 vec4：矩阵按列数占，数组再乘元素个数。
                     * 少算会让后一个 uniform 落进矩阵的槽里，GL 链接直接报重叠 */
                    uint32_t slot_count = 0;
                    const uint32_t count = CountVaryingLocations(decl.Type, structs, slot_count)
                        ? std::max(1u, slot_count) : 1u;
                    uniforms.Add(decl.Name, type_key(decl), count * decl.ArraySize, conflict);
                }
                else if (decl.Kind == SlotKind::Sampler)
                    samplers.Add(decl.Name, type_key(decl), decl.ArraySize, conflict);
                else if (is_varying(stage, decl))
                {
                    uint32_t count = 0;
                    if (!CountVaryingLocations(decl.Type, structs, count))
                        conflict = "'" + decl.Name + "': unknown varying type '" + decl.Type + "'";
                    else if (decl.Kind == SlotKind::Output)
                        varyings.Add(decl.Name, type_key(decl), count * decl.ArraySize, conflict);
                    else
                        fragment_inputs.push_back({ decl.Name, { type_key(decl), count * decl.ArraySize } });
                }
                return std::nullopt;
            }, scratch, error_msg);

            if (!parsed)
                return false;
            if (!conflict.empty())
            {
                error_msg = conflict;
                return false;
            }
        }

        for (const auto& [name, input] : fragment_inputs)
        {
            const auto output = varyings.Slots.find(name);
            if (output == varyings.Slots.end())
            {
                error_msg = "fragment input '" + name + "' has no vertex output with the same name";
                return false;
            }
            if (output->second.Type != input.first || output->second.Count != input.second)
            {
                error_msg = "'" + name + "' differs between stages: " + output->second.Type + " vs " + input.first;
                return false;
            }
        }

        if (samplers.Next > kMaxSamplerBindings)
        {
            error_msg = "too many samplers: " + std::to_string(samplers.Next) + " bindings, at most " +
                std::to_string(kMaxSamplerBindings);
            return false;
        }

        for (const ShaderStage stage : kStageOrder)
        {
            const auto source = stage_sources.find(stage);
            if (source == stage_sources.end())
                continue;

            RewriteSlotDecls(source->second, [&](const SlotDecl& decl) -> std::optional<std::string>
            {
                std::string slot;
                if (decl.Kind == SlotKind::LooseUniform)
                {
                    if (write_uniform_locations)
                        slot = "location = " + std::to_string(uniforms.Slots.at(decl.Name).Index);
                }
                else if (decl.Kind == SlotKind::Sampler)
                    slot = "binding = " + std::to_string(samplers.Slots.at(decl.Name).Index);
                else if (is_varying(stage, decl))
                    slot = "location = " + std::to_string(varyings.Slots.at(decl.Name).Index);
                else
                    return std::nullopt;

                std::string qualifiers = slot;
                if (!decl.Qualifiers.empty())
                    qualifiers += (qualifiers.empty() ? "" : ", ") + decl.Qualifiers;
                return decl.Indent + (qualifiers.empty() ? "" : "layout(" + qualifiers + ") ") + decl.Declaration;
            }, scratch, error_msg);
            source->second = std::move(scratch);
        }
        return true;
    }

    std::unordered_map<ShaderCompiler::ShaderStage, std::string> ShaderCompiler::LoadStageSources(
        const std::string& filepath, ShaderTarget target, const ShaderVariantDefines& defines,
        ShaderSourceInfo* info)
    {
        std::unordered_map<ShaderStage, std::string> stage_sources;

        std::vector<std::string> dependencies;
        std::vector<std::pair<std::string, std::string>> contents;
        IncludeContext context;
        if (info != nullptr)
        {
            context.Dependencies = &dependencies;
            context.Contents = &contents;
        }
        const std::string source = ReadFile(filepath, context);
        if (info != nullptr)
        {
            /* 读不到也要留下依赖表：文件被改坏或删掉时，热重载仍要知道该盯哪些文件 */
            info->Hash.clear();
            info->Dependencies = dependencies;
            info->RawHash = contents.size() == dependencies.size() ? HashContents(contents) : std::string{};
        }
        if (source.empty())
            return stage_sources;

        PreProcessShaderSrc(source, stage_sources);

        /* 变体声明从展开后的源码里读（可能写在 include 里）。按固定阶段顺序收集，
         * 同一维度在多个阶段声明时先到者定，结果不随容器遍历顺序变。 */
        std::vector<ShaderVariantSpec> variant_specs;
        for (const ShaderStage stage : { ShaderStage::Vertex, ShaderStage::Fragment, ShaderStage::Compute })
        {
            const auto stage_it = stage_sources.find(stage);
            if (stage_it == stage_sources.end())
                continue;

            for (const ShaderVariantSpec& spec : ShaderAnnotation::ParseVariantSpecs(stage_it->second))
            {
                const bool exists = std::any_of(variant_specs.begin(), variant_specs.end(),
                    [&spec](const ShaderVariantSpec& known) { return known.Name == spec.Name; });
                if (!exists)
                    variant_specs.push_back(spec);
            }
        }

        /* 补成声明全集再注入：声明过没请求的补 0，请求了没声明的丢掉。
         * 这样"全默认"的空表与显式写 0 是同一份源码，拼错的维度名也不会静默多编一份。 */
        std::vector<std::string> unknown_defines;
        const ShaderVariantDefines normalized =
            ShaderAnnotation::NormalizeVariantDefines(variant_specs, defines, &unknown_defines);

        /* 声明与取值的几处约定，都在这里一次报完 */
        ReportUnknownVariantDefines(filepath, unknown_defines);
        ReportVariantValueOutOfRange(filepath, defines);
        ValidateParamVariants(filepath, variant_specs);
        ReportVariantConditionalMisuse(filepath, stage_sources, variant_specs);

        InjectVariantDefines(stage_sources, normalized);
        PrepareStageSources(stage_sources, target, ExtractFilename(filepath));

        if (info != nullptr)
        {
            info->VariantSpecs = std::move(variant_specs);
            info->VariantDefines = normalized;
        }

        if (info != nullptr && !stage_sources.empty())
        {
            /* 阶段表是无序容器，拼接顺序不能直接遍历，否则同一份源码会有两个哈希 */
            std::string combined;
            for (const ShaderStage stage : { ShaderStage::Vertex, ShaderStage::Fragment, ShaderStage::Compute })
            {
                const auto it = stage_sources.find(stage);
                if (it != stage_sources.end())
                    combined += it->second;
            }
            info->Hash = ComputeHash(combined);
        }
        return stage_sources;
    }

    void ShaderCompiler::InjectVariantDefines(std::unordered_map<ShaderStage, std::string>& stage_sources,
        const ShaderVariantDefines& defines)
    {
        if (defines.empty())
            return;

        std::string prefix;
        for (const auto& entry : defines)
            prefix += "#define " + entry.first + " " + std::to_string(entry.second) + "\n";

        for (auto& entry : stage_sources)
        {
            std::string& source = entry.second;
            /* #version 必须是第一条非注释指令：前缀插在它那一行之后 */
            size_t insert = 0;
            if (const size_t version = source.find("#version"); version != std::string::npos)
            {
                const size_t line_end = source.find('\n', version);
                insert = (line_end == std::string::npos) ? source.size() : line_end + 1;
            }
            source.insert(insert, prefix);
        }
    }

    void ShaderCompiler::PrepareStageSources(std::unordered_map<ShaderStage, std::string>& stage_sources,
        ShaderTarget target, const std::string& name)
    {
#ifdef PLATFORM_WINDOWS
        const bool write_uniform_locations = target == ShaderTarget::OpenGL;
#else
        const bool write_uniform_locations = false;
        (void)target;
#endif
        std::string error_msg;
        if (!AssignShaderSlots(stage_sources, write_uniform_locations, error_msg))
            CORE_LOG_ERROR("Shader slot assignment failed ({}): {}", name, error_msg);
    }

    std::string ShaderCompiler::Compile(const std::string& source, ShaderStage stage, ShaderTarget target,
        const std::string& name)
    {
        std::string output;
        std::string error_msg;
        if (!Compile(source, stage, target, output, error_msg, name))
        {
            CORE_LOG_ERROR("ShaderCompiler failed: {}", error_msg);
            return "";
        }
        return output;
    }

    bool ShaderCompiler::Compile(const std::string& source, ShaderStage stage, ShaderTarget target,
        std::string& output, std::string& error_msg, const std::string& name)
    {
        const std::string hash = ComputeHash(source);
        const std::filesystem::path cache_dir = GetCacheDirectory(target);
        const std::string filename = (name.empty() ? "" : name + "_") + hash + "_" + ShaderStageToString(stage) + GetFileExtension(target);
        const std::string cache_path = (cache_dir / filename).string();

        /* 命中缓存不用排队：缓存文件整份写完才改名落位，读到的不会是半截 */
        if (TryLoadCache(cache_path, output))
        {
            CORE_LOG_INFO("ShaderCompiler cache hit: {}", cache_path);
            return true;
        }

        /* 真编译串行（见 CompileMutex 的说明）；排队期间别人可能已经编好写进去了 */
        std::lock_guard lock(CompileMutex());
        if (TryLoadCache(cache_path, output))
            return true;

        if (!CompileInternal(source, stage, target, output, error_msg, name))
            return false;

        SaveCache(cache_path, output);
        return true;
    }

    std::string ShaderCompiler::TargetToString(ShaderTarget target)
    {
        switch (target)
        {
        case ShaderTarget::Metal: return "Metal";
        case ShaderTarget::OpenGL: return "OpenGL";
        case ShaderTarget::Vulkan: return "Vulkan";
        }
        return "Unknown";
    }

    std::string ShaderCompiler::GetFileExtension(ShaderTarget target)
    {
        switch (target)
        {
        case ShaderTarget::Metal: return ".metal";
        case ShaderTarget::OpenGL: return ".glsl";
        case ShaderTarget::Vulkan: return ".spv";
        }
        return ".bin";
    }

    std::string ShaderCompiler::ComputeHash(const std::string& source)
    {
        /* 缓存键包含“翻译规则版本”：见 kTranslatorVersion 的说明 */
        const std::string key = std::string(kTranslatorVersion) + '\n' + source;
        const uint64_t hash = XXH64(key.c_str(), key.length(), 0);
        std::stringstream ss;
        ss << std::hex << std::uppercase << hash;
        return ss.str();
    }

    const char* ShaderCompiler::TranslatorVersion()
    {
        return kTranslatorVersion;
    }

    std::string ShaderCompiler::HashDependencies(const std::vector<std::string>& dependencies)
    {
        std::vector<std::pair<std::string, std::string>> contents;
        contents.reserve(dependencies.size());
        for (const std::string& path : dependencies)
        {
            std::ifstream in(path, std::ios::in | std::ios::binary);
            if (!in)
                return {};
            contents.emplace_back(path, std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
        }
        return HashContents(contents);
    }

    std::string ShaderCompiler::HashContents(const std::vector<std::pair<std::string, std::string>>& contents)
    {
        XXH64_state_t* state = XXH64_createState();
        if (state == nullptr)
            return {};

        XXH64_reset(state, 0);
        XXH64_update(state, kTranslatorVersion, std::strlen(kTranslatorVersion));

        for (const auto& [path, content] : contents)
        {
            /* 路径也进指纹：include 增删时哪怕内容一样也算变化 */
            XXH64_update(state, path.data(), path.size());
            XXH64_update(state, content.data(), content.size());
        }

        const uint64_t digest = XXH64_digest(state);
        XXH64_freeState(state);

        std::stringstream ss;
        ss << std::hex << std::uppercase << digest;
        return ss.str();
    }

    std::filesystem::path ShaderCompiler::GetCacheDirectory(ShaderTarget target)
    {
        /* 自检用：产物写到临时目录，不往项目缓存里留夹具和测试着色器 */
        if (const char* override_dir = std::getenv("HELIOS_SHADER_CACHE_DIR");
            override_dir != nullptr && override_dir[0] != '\0')
            return std::filesystem::path(override_dir) / TargetToString(target);

        return g_ProjectPath / "Cache/Shaders" / TargetToString(target);
    }

    bool ShaderCompiler::TryLoadCache(const std::string& cache_path, std::string& output)
    {
        std::ifstream in(cache_path, std::ios::in | std::ios::binary);
        if (!in.is_open())
            return false;

        in.seekg(0, std::ios::end);
        const size_t size = static_cast<size_t>(in.tellg());
        in.seekg(0, std::ios::beg);

        output.resize(size);
        in.read(output.data(), size);
        in.close();

        /* 命中就是"这份产物还在用"：刷新修改时间，清理按它判多久没被读过。
         * 一天最多写一次，避免每次启动给几百个文件各改一遍时间戳。 */
        std::error_code error;
        const auto now = std::filesystem::file_time_type::clock::now();
        const auto stamp = std::filesystem::last_write_time(cache_path, error);
        if (!error && now - stamp > std::chrono::hours(24))
            std::filesystem::last_write_time(cache_path, now, error);
        return true;
    }

    void ShaderCompiler::SaveCache(const std::string& cache_path, const std::string& output)
    {
        std::filesystem::path dir = std::filesystem::path(cache_path).parent_path();
        if (!std::filesystem::exists(dir))
            std::filesystem::create_directories(dir);

        /* 先写临时文件再改名：改名是原子的，别的线程或进程不会读到写了一半的缓存 */
        const std::string temp_path = cache_path + ".tmp"
            + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()))
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        {
            std::ofstream out(temp_path, std::ios::out | std::ios::binary);
            if (!out.is_open())
                return;
            out.write(output.data(), static_cast<std::streamsize>(output.size()));
            if (!out.good())
            {
                out.close();
                std::error_code ignored;
                std::filesystem::remove(temp_path, ignored);
                return;
            }
        }

        std::error_code error;
        std::filesystem::rename(temp_path, cache_path, error);
        if (error)
            std::filesystem::remove(temp_path, error);

        TrimCache(dir);
    }

    bool ShaderCompiler::CompileInternal(const std::string& source, ShaderStage stage, ShaderTarget target,
        std::string& output, std::string& error_msg, const std::string& name)
    {
        if (target == ShaderTarget::Metal)
        {
            std::vector<uint32_t> spirv;
            std::string processed_source;
            if (!CompileGLSLToSPIRV(source, stage, spirv, processed_source, error_msg))
                return false;
            ApplyDepthTextureAnnotations(processed_source, CollectDepthTextureAnnotations(source), spirv);
            if (!SPIRVToTarget(spirv, source, stage, target, output, error_msg))
                return false;
            PrependSourceInfo(output, name, stage, target);
            return true;
        }

        if (target == ShaderTarget::OpenGL)
        {
            /* OpenGL 直接使用 GLSL 源码，无需跨平台翻译 */
            output = source;
            PrependSourceInfo(output, name, stage, target);
            return true;
        }

        if (target == ShaderTarget::Vulkan)
        {
            std::vector<uint32_t> spirv;
            std::string processed_source;
            if (!CompileGLSLToSPIRV(source, stage, spirv, processed_source, error_msg))
                return false;
            ApplyDepthTextureAnnotations(processed_source, CollectDepthTextureAnnotations(source), spirv);
            output.assign(reinterpret_cast<const char*>(spirv.data()), spirv.size() * sizeof(uint32_t));
            /* Vulkan 输出为二进制 SPIR-V，不写入文本注释 */
            return true;
        }

        error_msg = "Unsupported shader target";
        return false;
    }

    void ShaderCompiler::PrependSourceInfo(std::string& output, const std::string& name, ShaderStage stage, ShaderTarget target)
    {
        std::string header = "// Translated from Helios ShaderCompiler\n";
        if (!name.empty())
            header += "// Source: " + name + "\n";
        header += "// Stage: " + std::string(ShaderStageToString(stage)) + "\n";
        header += "// Target: " + TargetToString(target) + "\n";
        header += "\n";
        output = header + output;
    }

    bool ShaderCompiler::CompileGLSLToSPIRV(const std::string& glsl_source, ShaderStage stage,
        std::vector<uint32_t>& spirv, std::string& processed_source, std::string& error_msg)
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

        using namespace glslang;

        static std::once_flag s_InitFlag;
        std::call_once(s_InitFlag, []() { glslang::InitializeProcess(); });

        /* glslang 编译为 SPIR-V 需要 #version 450 core，并需要 sampler 有 layout(binding=X) */
        processed_source = glsl_source;
        {
            static const std::regex version_regex("#version\\s+410\\s+core");
            processed_source = std::regex_replace(processed_source, version_regex, "#version 450 core");
        }

        {
            /* sampler / image 统一放到 set = SamplerDescriptorSet（跟 uniform block 的 set = 0 分开）。
             * SPIR-V 的 (set, binding) 必须全局唯一（同一个键上 buffer / texture / sampler 共用记录）；
             * 采样器在 MSL 里的索引仍等于自己的 binding。 */
            static constexpr uint32_t SamplerDescriptorSet = 1;

            static const std::regex existing_binding_regex("layout\\s*\\(\\s*[^\\)]*binding\\s*=\\s*(\\d+)[^\\)]*\\)\\s*uniform\\s+(sampler\\w+|image\\w+)");
            uint32_t next_binding = 0;
            for (std::sregex_iterator it(processed_source.begin(), processed_source.end(), existing_binding_regex), end; it != end; ++it)
                next_binding = std::max(next_binding, static_cast<uint32_t>(std::stoul(it->str(1))) + 1);

            /* 可选的 layout(...) 前缀 + uniform samplerXxx/imageXxx 声明 */
            static const std::regex sampler_regex(
                R"((layout\s*\(([^)]*)\)\s+)?uniform\s+(sampler\w+|image\w+)\s+(\w+))");
            static const std::regex binding_regex(R"(binding\s*=\s*(\d+))");
            static const std::regex set_regex(R"(\bset\s*=)");

            std::string replaced;
            size_t last_pos = 0;
            for (std::sregex_iterator it(processed_source.begin(), processed_source.end(), sampler_regex), end; it != end; ++it)
            {
                replaced += processed_source.substr(last_pos, it->position() - last_pos);

                const std::string qualifiers = it->str(2);
                std::smatch set_match;
                if (std::regex_search(qualifiers, set_match, set_regex))
                {
                    /* 已经显式指定了 set，保持原样 */
                    replaced += it->str(0);
                }
                else
                {
                    std::smatch binding_match;
                    std::string new_qualifiers = qualifiers;
                    if (!std::regex_search(qualifiers, binding_match, binding_regex))
                        new_qualifiers += (new_qualifiers.empty() ? "" : ", ") + std::string("binding = ") + std::to_string(next_binding++);

                    replaced += "layout(set = " + std::to_string(SamplerDescriptorSet) +
                        (new_qualifiers.empty() ? std::string() : ", " + new_qualifiers) +
                        ") uniform " + it->str(3) + " " + it->str(4);
                }

                last_pos = it->position() + it->length();
            }
            replaced += processed_source.substr(last_pos);
            processed_source = std::move(replaced);
        }

        EShLanguage esh_stage = EShLangVertex;
        if (stage == ShaderStage::Fragment)
            esh_stage = EShLangFragment;
        else if (stage == ShaderStage::Compute)
            esh_stage = EShLangCompute;

        TShader shader(esh_stage);
        const char* source = processed_source.c_str();
        shader.setStrings(&source, 1);
        shader.setEnvInput(EShSourceGlsl, esh_stage, EShClientVulkan, 100);
        shader.setEnvClient(EShClientVulkan, EShTargetVulkan_1_0);
        shader.setEnvTarget(EShTargetSpv, EShTargetSpv_1_0);

        /* 引擎 GLSL 沿用 OpenGL 写法：裸 uniform 可以声明在全局作用域（默认的 Vulkan 规则会拒绝，
         * 打开 relaxed 规则后聚合成"默认 uniform 块"）。这个块在源码里没有显式 binding，缺省值会跟
         * binding = 0 撞键（后设的覆盖前设的），所以显式指定成 DefaultUniformBufferBinding（Metal 后端预留）。 */
        shader.setEnvInputVulkanRulesRelaxed();
        shader.setGlobalUniformSet(0);
        shader.setGlobalUniformBinding(MetalBinding::DefaultUniformBufferBinding);

        shader.setEntryPoint("main");
        shader.setInvertY(false);

        TBuiltInResource resources = *GetDefaultResources();
        if (!shader.parse(&resources, 100, false, EShMsgDefault))
        {
            error_msg = std::string(shader.getInfoLog()) + "\n" + shader.getInfoDebugLog();
            return false;
        }

        TProgram program;
        program.addShader(&shader);
        if (!program.link(EShMsgDefault))
        {
            error_msg = std::string(program.getInfoLog()) + "\n" + program.getInfoDebugLog();
            return false;
        }

        GlslangToSpv(*program.getIntermediate(esh_stage), spirv);
        return true;
    }

    std::vector<std::string> ShaderCompiler::CollectDepthTextureAnnotations(const std::string& source)
    {
        /* `// @depth-texture <采样器名>`：深度纹理采样标注（约定见头文件） */
        static const std::regex annotation_regex(R"(@depth-texture\s+([A-Za-z_]\w*))");

        std::vector<std::string> names;
        for (std::sregex_iterator it(source.begin(), source.end(), annotation_regex), end; it != end; ++it)
            names.push_back((*it)[1].str());
        return names;
    }

    void ShaderCompiler::ApplyDepthTextureAnnotations(const std::string& processed_source,
        const std::vector<std::string>& annotations, std::vector<uint32_t>& spirv)
    {
        if (annotations.empty())
            return;

        /* 预处理后源码中所有采样器都带显式 binding：建立 名字 -> binding 映射 */
        static const std::regex sampler_regex(R"(layout\s*\(([^)]*)\)\s*uniform\s+sampler\w+\s+([A-Za-z_]\w*))");
        static const std::regex binding_regex(R"(binding\s*=\s*(\d+))");

        std::unordered_map<std::string, uint32_t> sampler_bindings;
        for (std::sregex_iterator it(processed_source.begin(), processed_source.end(), sampler_regex), end; it != end; ++it)
        {
            const std::string qualifiers = it->str(1);
            std::smatch binding_match;
            if (std::regex_search(qualifiers, binding_match, binding_regex))
                sampler_bindings[it->str(2)] = static_cast<uint32_t>(std::stoul(binding_match[1].str()));
        }

        for (const std::string& name : annotations)
        {
            const auto iter = sampler_bindings.find(name);
            if (iter == sampler_bindings.end())
            {
                CORE_LOG_ERROR("ShaderCompiler: @depth-texture '{}' has no matching sampler declaration", name);
                continue;
            }

            if (!MarkImageAsDepth(spirv, iter->second))
                CORE_LOG_ERROR("ShaderCompiler: @depth-texture '{}' (binding {}) image not found in SPIR-V",
                    name, iter->second);
        }
    }

    bool ShaderCompiler::MarkImageAsDepth(std::vector<uint32_t>& spirv, uint32_t binding)
    {
        /* SPIR-V 指令 = [opcode | wordCount][操作数...]，模块头占前 5 个字 */
        constexpr uint32_t OpTypeImage = 25;
        constexpr uint32_t OpTypeSampledImage = 27;
        constexpr uint32_t OpTypePointer = 32;
        constexpr uint32_t OpFunction = 54;
        constexpr uint32_t OpVariable = 59;
        constexpr uint32_t OpDecorate = 71;
        constexpr uint32_t DecorationBinding = 33;

        const auto find_by_result_id = [&spirv](uint32_t opcode, uint32_t result_id) -> const uint32_t*
        {
            for (size_t i = 5; i < spirv.size();)
            {
                const uint32_t instruction = spirv[i];
                const uint32_t word_count = instruction >> 16;
                if ((instruction & 0xFFFFu) == opcode && word_count >= 2 && spirv[i + 1] == result_id)
                    return &spirv[i];
                i += word_count;
            }
            return nullptr;
        };

        /* 1) binding 命中的变量 id（组合采样器拆成 image + sampler 两个变量，都会命中） */
        std::vector<uint32_t> variables;
        for (size_t i = 5; i < spirv.size();)
        {
            const uint32_t instruction = spirv[i];
            const uint32_t word_count = instruction >> 16;
            if ((instruction & 0xFFFFu) == OpDecorate && word_count >= 4
                && spirv[i + 2] == DecorationBinding && spirv[i + 3] == binding)
                variables.push_back(spirv[i + 1]);
            i += word_count;
        }

        /* 2) 变量 -> OpTypePointer -> (OpTypeSampledImage) -> OpTypeImage，记下各条指令的位置。
         * OpVariable 的变量 id 在 word2（word1 是结果类型），需专用遍历。 */
        struct ImageEntry
        {
            size_t VariableInstruction;
            size_t PointerInstruction;
            size_t SampledImageInstruction;
            size_t ImageInstruction;
            uint32_t ImageTypeId;
        };

        constexpr size_t kNoInstruction = static_cast<size_t>(-1);
        std::vector<ImageEntry> entries;
        /* 每个图像类型被几个变量引用，共用时不能就地改（见第 3 步） */
        std::unordered_map<uint32_t, uint32_t> image_references;

        for (size_t i = 5; i < spirv.size();)
        {
            const uint32_t instruction = spirv[i];
            const uint32_t word_count = instruction >> 16;
            if ((instruction & 0xFFFFu) == OpVariable && word_count >= 4)
            {
                const uint32_t ptr_type = spirv[i + 1];
                const uint32_t variable_id = spirv[i + 2];

                const uint32_t* pointer = find_by_result_id(OpTypePointer, ptr_type);
                if (pointer == nullptr)
                {
                    i += word_count;
                    continue;
                }

                size_t sampled_image_index = kNoInstruction;
                uint32_t image_type_id = pointer[3];
                const uint32_t* sampled_image = find_by_result_id(OpTypeSampledImage, image_type_id);
                if (sampled_image != nullptr)
                {
                    sampled_image_index = static_cast<size_t>(sampled_image - spirv.data());
                    image_type_id = sampled_image[2];
                }

                /* 指向 OpTypeSampler 的变量（组合采样器拆出来的那一半）这里找不到图像，跳过 */
                const uint32_t* image = find_by_result_id(OpTypeImage, image_type_id);
                if (image == nullptr)
                {
                    i += word_count;
                    continue;
                }

                ++image_references[image_type_id];
                const bool is_depth = std::find(variables.begin(), variables.end(), variable_id) != variables.end();
                if (is_depth)
                {
                    entries.push_back(ImageEntry{ i, static_cast<size_t>(pointer - spirv.data()),
                        sampled_image_index, static_cast<size_t>(image - spirv.data()), image_type_id });
                }
            }
            i += word_count;
        }

        /* 3) 独占就就地置 Depth（OpTypeImage: [result, sampledType, Dim, Depth, ...]）；
         * 被别的采样器共用时复制一条类型链再改 —— 彩色采样器和深度采样器的 OpTypeImage
         * 完全一样，glslang 会去重成同一个类型，就地改会把彩色采样器一起标成深度图。 */
        bool patched = false;
        std::vector<uint32_t> appended;
        std::vector<std::pair<size_t, uint32_t>> repoint;

        const auto copy_instruction = [&spirv](size_t index)
        {
            const size_t word_count = spirv[index] >> 16;
            return std::vector<uint32_t>(spirv.begin() + static_cast<ptrdiff_t>(index),
                spirv.begin() + static_cast<ptrdiff_t>(index + word_count));
        };

        for (const ImageEntry& entry : entries)
        {
            if (image_references[entry.ImageTypeId] <= 1)
            {
                spirv[entry.ImageInstruction + 4] = 1;
                patched = true;
                continue;
            }

            /* 新的类型 id 直接取模块头里的 id 上界往下发 */
            const uint32_t new_image_id = spirv[3]++;
            std::vector<uint32_t> image = copy_instruction(entry.ImageInstruction);
            image[1] = new_image_id;
            image[4] = 1;
            appended.insert(appended.end(), image.begin(), image.end());

            uint32_t pointer_target = new_image_id;
            if (entry.SampledImageInstruction != kNoInstruction)
            {
                const uint32_t new_sampled_id = spirv[3]++;
                std::vector<uint32_t> sampled = copy_instruction(entry.SampledImageInstruction);
                sampled[1] = new_sampled_id;
                sampled[2] = new_image_id;
                appended.insert(appended.end(), sampled.begin(), sampled.end());
                pointer_target = new_sampled_id;
            }

            const uint32_t new_pointer_id = spirv[3]++;
            std::vector<uint32_t> pointer = copy_instruction(entry.PointerInstruction);
            pointer[1] = new_pointer_id;
            pointer[3] = pointer_target;
            appended.insert(appended.end(), pointer.begin(), pointer.end());

            repoint.emplace_back(entry.VariableInstruction, new_pointer_id);
            patched = true;
        }

        for (const auto& [variable_instruction, pointer_id] : repoint)
            spirv[variable_instruction + 1] = pointer_id;

        if (!appended.empty())
        {
            /* 类型定义只能出现在第一个函数之前 */
            size_t insert_at = spirv.size();
            for (size_t i = 5; i < spirv.size(); i += (spirv[i] >> 16))
            {
                if ((spirv[i] & 0xFFFFu) == OpFunction)
                {
                    insert_at = i;
                    break;
                }
            }
            spirv.insert(spirv.begin() + static_cast<ptrdiff_t>(insert_at), appended.begin(), appended.end());
        }
        return patched;
    }

    bool ShaderCompiler::SPIRVToTarget(const std::vector<uint32_t>& spirv, const std::string& glsl_source,
        ShaderStage stage, ShaderTarget target, std::string& output, std::string& error_msg)
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

        if (target != ShaderTarget::Metal)
        {
            error_msg = "SPIRVToTarget only supports Metal target currently";
            return false;
        }

#ifdef PLATFORM_MACOS
        try
        {
            spirv_cross::CompilerMSL compiler(spirv);
            spirv_cross::CompilerMSL::Options options;
            options.set_msl_version(spirv_cross::CompilerMSL::Options::make_msl_version(2, 3));
            options.platform = spirv_cross::CompilerMSL::Options::macOS;
            options.enable_decoration_binding = true;
            compiler.set_msl_options(options);

            auto entry_points = compiler.get_entry_points_and_stages();
            if (!entry_points.empty())
            {
                const char* target_entry = (stage == ShaderStage::Vertex) ? "vertex_main" :
                                           (stage == ShaderStage::Fragment) ? "fragment_main" : "compute_main";
                compiler.rename_entry_point("main", target_entry, entry_points[0].execution_model);
                compiler.set_entry_point(target_entry, entry_points[0].execution_model);
            }

            /* Uniform Buffer 的 binding → Metal buffer 索引：binding = N 对应
             * [[buffer(N + UniformBufferBase)]]，跟顶点缓冲槽位错开。裸 uniform 聚合出的"默认块"没有
             * 显式 binding（编译时指定为 DefaultUniformBufferBinding），按"源码里有没有显式声明"识别、
             * 映射到 DefaultUniformBufferIndex。 */
            const auto explicit_bindings = CollectExplicitUniformBlockBindings(glsl_source);
            const auto resources = compiler.get_shader_resources();
            const spv::ExecutionModel model = compiler.get_execution_model();
            for (const auto& resource : resources.uniform_buffers)
            {
                const uint32_t set = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
                const uint32_t binding = compiler.get_decoration(resource.id, spv::DecorationBinding);

                const bool explicit_binding = explicit_bindings.find(binding) != explicit_bindings.end();
                const uint32_t msl_buffer = explicit_binding
                    ? MetalBinding::ToBufferIndex(binding)
                    : MetalBinding::DefaultUniformBufferIndex;

                spirv_cross::MSLResourceBinding msl_binding;
                msl_binding.stage = model;
                msl_binding.basetype = spirv_cross::SPIRType::Struct;
                msl_binding.desc_set = set;
                msl_binding.binding = binding;
                msl_binding.count = 1;
                msl_binding.msl_buffer = msl_buffer;
                msl_binding.msl_texture = 0;
                msl_binding.msl_sampler = 0;
                compiler.add_msl_resource_binding(msl_binding);
            }

            output = compiler.compile();
            return true;
        }
        catch (const std::exception& e)
        {
            error_msg = e.what();
            return false;
        }
#else
        /* MSL 由 Metal 后端所在平台生成：非 macOS 平台不参与 Metal 着色器编译 */
        (void)spirv;
        (void)glsl_source;
        error_msg = "MSL generation is only available on the Metal backend platform";
        return false;
#endif
    }
}
