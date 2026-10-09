#include "Pch.h"
#include "ShaderCompiler.h"

#include "Helios/Common/Utils.h"
#include "Helios/Scene/SceneCommon.h"

#include <xxhash.h>
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
