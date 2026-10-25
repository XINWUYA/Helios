#include "Pch.h"
#include "ShaderCompiler.h"

#include "Helios/Common/Utils.h"
#include "Helios/Scene/SceneCommon.h"

#include <xxhash.h>
#include <functional>
#include <optional>
#include <regex>
#include <sstream>
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
            static const std::regex uniform_regex(
                R"(^(\s*)(?:layout\s*\(([^)]*)\)\s*)?(uniform\s+(?:(?:highp|mediump|lowp)\s+)?([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*(?:\[\s*([^\]]*?)\s*\])?\s*([=;,].*))$)");
            static const std::regex varying_regex(
                R"(^(\s*)(?:layout\s*\(([^)]*)\)\s*)?((?:(?:flat|noperspective|smooth|centroid|sample|invariant)\s+)*(in|out)\s+(?:(?:flat|noperspective|smooth|centroid|sample|highp|mediump|lowp)\s+)*([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*(?:\[\s*([^\]]*?)\s*\])?\s*(;.*))$)");
            static const std::regex basic_type_regex(R"((?:bool|int|uint|float|double|[biud]?vec[234]|d?mat[234](x[234])?))");
            static const std::regex slot_regex(R"(^\s*(location|binding)\s*=)");
            static const std::regex digits_regex(R"(\d+)");

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

                std::smatch match;
                SlotDecl decl;
                bool found = false;
                std::string qualifiers;
                std::string array_text;
                bool has_array = false;
                if (!commented && body.find("uniform") != std::string::npos && std::regex_match(body, match, uniform_regex))
                {
                    const std::string type = match.str(4);
                    if (IsSamplerType(type))
                    {
                        decl.Kind = SlotKind::Sampler;
                        found = true;
                    }
                    else if (!IsOpaqueUniformType(type))
                    {
                        if (match.str(7)[0] == ',')
                        {
                            error_msg = "uniform '" + match.str(5) + "': declare one uniform per line";
                            return false;
                        }
                        if (!std::regex_match(type, basic_type_regex))
                        {
                            error_msg = "uniform '" + match.str(5) + "': type '" + type +
                                "' is not a basic type, use a uniform block for structs";
                            return false;
                        }
                        decl.Kind = SlotKind::LooseUniform;
                        found = true;
                    }

                    if (found)
                    {
                        decl.Indent = match.str(1);
                        qualifiers = match.str(2);
                        decl.Declaration = match.str(3);
                        decl.Type = type;
                        decl.Name = match.str(5);
                        has_array = match[6].matched;
                        array_text = match.str(6);
                    }
                }
                else if (!commented && std::regex_match(body, match, varying_regex))
                {
                    decl.Kind = match.str(4) == "in" ? SlotKind::Input : SlotKind::Output;
                    decl.Indent = match.str(1);
                    qualifiers = match.str(2);
                    decl.Declaration = match.str(3);
                    decl.Type = match.str(5);
                    decl.Name = match.str(6);
                    has_array = match[7].matched;
                    array_text = match.str(7);
                    found = true;
                }

                if (found)
                {
                    if (has_array)
                    {
                        if (!std::regex_match(array_text, digits_regex))
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
                        if (item.empty() || std::regex_search(item, slot_regex))
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

    std::string ShaderCompiler::ReadFile(const std::string& filepath)
    {
        PROFILE_FUNCTION();

        std::string result;
        std::ifstream in(filepath, std::ios::in | std::ios::binary);
        if (in)
        {
            static const std::regex regex_quote("^[ ]*#[ ]*include[ ]+\"([^\"]+)\".*");
            static const std::regex regex_angle("^[ ]*#[ ]*include[ ]+<([^>]+)>.*");
            std::smatch matches;
            std::string line_str;
            while (std::getline(in, line_str))
            {
                if (line_str.find("#include") != line_str.npos)
                {
                    std::string include_file;
                    bool matched = false;
                    if (std::regex_search(line_str, matches, regex_quote))
                    {
                        include_file = matches.str(1);
                        matched = true;
                    }
                    else if (std::regex_search(line_str, matches, regex_angle))
                    {
                        include_file = matches.str(1);
                        matched = true;
                    }

                    if (matched)
                    {
                        std::string base_dir = ExtractFileBaseDir(filepath);
                        result += ReadFile(base_dir + include_file) + '\n';
                    }
                    else
                    {
                        result += line_str + '\n';
                    }
                }
                else
                {
                    result += line_str + '\n';
                }
            }
        }
        else
        {
            CORE_LOG_ERROR("Could not open shader file: {}", filepath);
        }

        return result;
    }

    void ShaderCompiler::PreProcessShaderSrc(const std::string& source,
        std::unordered_map<ShaderStage, std::string>& shader_sources)
    {
        PROFILE_FUNCTION();

        const char* type_token = "#type";
        const size_t type_token_length = strlen(type_token);
        size_t pos = source.find(type_token, 0);
        while (pos != std::string::npos)
        {
            const size_t eol = source.find_first_of("\r\n", pos);
            if (eol == std::string::npos)
            {
                CORE_LOG_ERROR("Shader syntax error: #type declaration has no newline.");
                ASSERT(false, "Syntax error");
                return;
            }

            const size_t begin = pos + type_token_length + 1;
            std::string type = source.substr(begin, eol - begin);
            ShaderStage stage = StringToShaderStage(type);

            const size_t next_line_pos = source.find_first_not_of("\r\n", eol);
            if (next_line_pos == std::string::npos)
            {
                CORE_LOG_ERROR("Shader syntax error: #type declaration has no shader code.");
                ASSERT(false, "Syntax error");
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
                    uniforms.Add(decl.Name, type_key(decl), decl.ArraySize, conflict);
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
        const std::string& filepath, ShaderTarget target)
    {
        std::unordered_map<ShaderStage, std::string> stage_sources;
        const std::string source = ReadFile(filepath);
        if (source.empty())
            return stage_sources;

        PreProcessShaderSrc(source, stage_sources);
        PrepareStageSources(stage_sources, target, ExtractFilename(filepath));
        return stage_sources;
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

        if (TryLoadCache(cache_path, output))
        {
            CORE_LOG_INFO("ShaderCompiler cache hit: {}", cache_path);
            return true;
        }

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
        /* 缓存键包含“翻译规则版本”：翻译结果由 glslang 的编译规则、spirv-cross 的
         * MSL 版本以及 MetalBinding 的绑定槽位约定共同决定，这些配置变化时源码哈希不变，
         * 旧缓存会被继续复用。因此让版本号参与哈希，修改翻译规则时请同时递增它。 */
        static constexpr const char* kTranslatorVersion = "helios-shader-translator-v5";

        const std::string key = std::string(kTranslatorVersion) + '\n' + source;
        const uint64_t hash = XXH64(key.c_str(), key.length(), 0);
        std::stringstream ss;
        ss << std::hex << std::uppercase << hash;
        return ss.str();
    }

    std::filesystem::path ShaderCompiler::GetCacheDirectory(ShaderTarget target)
    {
        return g_AssetsPath / "Cache/Shaders" / TargetToString(target);
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
        return true;
    }

    void ShaderCompiler::SaveCache(const std::string& cache_path, const std::string& output)
    {
        std::filesystem::path dir = std::filesystem::path(cache_path).parent_path();
        if (!std::filesystem::exists(dir))
            std::filesystem::create_directories(dir);

        std::ofstream out(cache_path, std::ios::out | std::ios::binary);
        if (out.is_open())
        {
            out.write(output.data(), output.size());
            out.flush();
            out.close();
        }
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
        PROFILE_FUNCTION();

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

        /* 2) 变量 -> OpTypePointer -> (OpTypeSampledImage) -> OpTypeImage 的结果 id。
         * OpVariable 的变量 id 在 word2（word1 是结果类型），需专用遍历。 */
        std::vector<uint32_t> image_type_ids;
        for (size_t i = 5; i < spirv.size();)
        {
            const uint32_t instruction = spirv[i];
            const uint32_t word_count = instruction >> 16;
            if ((instruction & 0xFFFFu) == OpVariable && word_count >= 4)
            {
                const uint32_t ptr_type = spirv[i + 1];
                const uint32_t variable_id = spirv[i + 2];
                bool hit = false;
                for (const uint32_t variable : variables)
                    hit = hit || variable == variable_id;
                if (hit)
                {
                    const uint32_t* pointer = find_by_result_id(OpTypePointer, ptr_type);
                    if (pointer == nullptr)
                        continue;

                    uint32_t image_type_id = pointer[3];
                    if (const uint32_t* sampled_image = find_by_result_id(OpTypeSampledImage, image_type_id))
                        image_type_id = sampled_image[2];
                    image_type_ids.push_back(image_type_id);
                }
            }
            i += word_count;
        }

        /* 3) 把这些图像的 Depth 操作数置 1（OpTypeImage: [result, sampledType, Dim, Depth, ...]） */
        bool patched = false;
        for (size_t i = 5; i < spirv.size();)
        {
            const uint32_t instruction = spirv[i];
            const uint32_t word_count = instruction >> 16;
            if ((instruction & 0xFFFFu) == OpTypeImage && word_count >= 9)
            {
                for (const uint32_t image_type_id : image_type_ids)
                {
                    if (spirv[i + 1] == image_type_id)
                    {
                        spirv[i + 4] = 1;
                        patched = true;
                    }
                }
            }
            i += word_count;
        }
        return patched;
    }

    bool ShaderCompiler::SPIRVToTarget(const std::vector<uint32_t>& spirv, const std::string& glsl_source,
        ShaderStage stage, ShaderTarget target, std::string& output, std::string& error_msg)
    {
        PROFILE_FUNCTION();

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
