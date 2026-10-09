#ifdef PLATFORM_MACOS

#include "Pch.h"
#include "MetalShader.h"
#include "MetalCommon.h"
#include "MetalRenderAPI.h"
#include "MetalVertexArray.h"
#include "Helios/Renderer/Renderer.h"
#include "Helios/Renderer/ShaderCompiler.h"

#include <cstdlib>
#include <cstring>

namespace Helios
{
    namespace
    {
        /* MSL 中 constant 地址空间下各类型的大小与对齐。float3 在 constant
         * 缓冲区中占据 16 字节（含填充），这是 SPIRV-Cross 生成布局与引擎端
         * 偏移计算共同遵循的前提。 */
        [[nodiscard]] uint32_t GetMaterialTypeAlignment(const std::string& type)
        {
            if (type == "float" || type == "int" || type == "uint" || type == "bool") return 4;
            if (type == "float2" || type == "int2" || type == "uint2") return 8;
            if (type.rfind("packed_", 0) == 0) return 4;
            return 16;
        }

        [[nodiscard]] uint32_t GetMaterialTypeSize(const std::string& type)
        {
            if (type == "float" || type == "int" || type == "uint" || type == "bool") return 4;
            if (type == "float2" || type == "int2" || type == "uint2") return 8;
            if (type == "float3" || type == "int3" || type == "uint3") return 16;
            if (type == "float4" || type == "int4" || type == "uint4") return 16;
            if (type == "float4x4") return 64;
            if (type == "float3x3") return 48;
            if (type == "packed_float2") return 8;
            if (type == "packed_float3") return 12;
            if (type == "packed_float4") return 16;
            return 16;
        }

        /* MSL 形参上的 [[texture(N)]] / [[sampler(N)]] 是资源槽位的权威来源 */
        const std::regex& GetTextureRegex()
        {
            static const std::regex re(
                R"(([A-Za-z_][A-Za-z0-9_]*)\s*\[\[\s*texture\s*\(\s*(\d+)\s*\)\s*\]\])");
            return re;
        }

        const std::regex& GetSamplerRegex()
        {
            static const std::regex re(
                R"(([A-Za-z_][A-Za-z0-9_]*)\s*\[\[\s*sampler\s*\(\s*(\d+)\s*\)\s*\]\])");
            return re;
        }

        /* SPIRV-Cross 为从组合采样器中分离出的采样器追加的后缀 */
        constexpr const char* kSamplerSuffix = "Smplr";

        /* GLSL 裸 uniform 默认初始值解析：MSL 的 constant 结构体成员不支持初始化器，GLSL 写在
         * uniform 上的默认值经 glslang → SPIRV-Cross 会丢。Metal 后端创建材质块时用这张表还原，
         * 不然没通过材质写入的 uniform 会一直读到全零。 */

        [[nodiscard]] std::string TrimToken(const std::string& str)
        {
            const size_t begin = str.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos)
                return {};
            const size_t end = str.find_last_not_of(" \t\r\n");
            return str.substr(begin, end - begin + 1);
        }

        /* 把 GLSL 初始化表达式序列化为 MSL 布局下的字节序列。
         * 支持 float/vecN、int/ivecN、uint/uvecN、bool/bvecN 以及构造函数
         * splat（如 vec4(1.0)）；解析失败返回空，调用方回退到零初始化。 */
        [[nodiscard]] std::vector<uint8_t> SerializeUniformDefaultValue(
            const std::string& type, const std::string& init_expr)
        {
            enum class Comp : uint8_t { Float, Int, Uint, Bool };
            struct TypeInfo { Comp comp; uint32_t count; };
            static const std::unordered_map<std::string, TypeInfo> types = {
                {"float", {Comp::Float, 1}}, {"vec2", {Comp::Float, 2}},
                {"vec3",  {Comp::Float, 3}}, {"vec4", {Comp::Float, 4}},
                {"int",   {Comp::Int, 1}},   {"ivec2", {Comp::Int, 2}},
                {"ivec3", {Comp::Int, 3}},   {"ivec4", {Comp::Int, 4}},
                {"uint",  {Comp::Uint, 1}},  {"uvec2", {Comp::Uint, 2}},
                {"uvec3", {Comp::Uint, 3}},  {"uvec4", {Comp::Uint, 4}},
                {"bool",  {Comp::Bool, 1}},  {"bvec2", {Comp::Bool, 2}},
                {"bvec3", {Comp::Bool, 3}},  {"bvec4", {Comp::Bool, 4}},
            };

            const auto tit = types.find(type);
            if (tit == types.end())
                return {};
            /* 不用结构化绑定：lambda 捕获结构化绑定是 C++20 扩展 */
            const Comp comp = tit->second.comp;
            const uint32_t count = tit->second.count;

            /* 提取构造函数括号内的分量列表；无括号则视为单个标量字面量 */
            std::string values_str = TrimToken(init_expr);
            const auto lparen = values_str.find('(');
            if (lparen != std::string::npos)
            {
                const auto rparen = values_str.rfind(')');
                if (rparen == std::string::npos || rparen < lparen)
                    return {};
                values_str = values_str.substr(lparen + 1, rparen - lparen - 1);
            }

            /* 按逗号切分分量 */
            std::vector<std::string> tokens;
            size_t pos = 0;
            while (true)
            {
                const size_t comma = values_str.find(',', pos);
                tokens.emplace_back(TrimToken(values_str.substr(pos,
                    comma == std::string::npos ? std::string::npos : comma - pos)));
                if (comma == std::string::npos)
                    break;
                pos = comma + 1;
            }

            /* GLSL 构造函数 splat：vec4(1.0) 表示全分量取该值。
             * 注意必须先拷贝：assign 会销毁旧元素，直接传 tokens[0]
             * （对自身元素的引用）会悬空产生垃圾数据。 */
            if (tokens.size() == 1 && count > 1)
            {
                const std::string splat_value = tokens[0];
                tokens.assign(count, splat_value);
            }

            if (tokens.size() != count)
                return {};

            /* 逐分量解析为 4 字节（bool 在 constant 缓冲区中按 4 字节布局，
             * 与 GetMaterialTypeSize 的约定一致） */
            const auto parse_token = [comp](const std::string& token, uint8_t* out) -> bool
            {
                if (token.empty())
                    return false;

                switch (comp)
                {
                case Comp::Float:
                    {
                        /* 去掉字面量后缀 f/F */
                        std::string t = token;
                        if (t.back() == 'f' || t.back() == 'F')
                            t.pop_back();
                        char* end = nullptr;
                        const float value = std::strtof(t.c_str(), &end);
                        if (end == t.c_str())
                            return false;
                        std::memcpy(out, &value, sizeof(value));
                        return true;
                    }
                case Comp::Int:
                    {
                        char* end = nullptr;
                        /* base = 0 自动识别十进制/八进制/十六进制，与 GLSL 字面量语义一致 */
                        const long value = std::strtol(token.c_str(), &end, 0);
                        if (end == token.c_str())
                            return false;
                        const int32_t v = static_cast<int32_t>(value);
                        std::memcpy(out, &v, sizeof(v));
                        return true;
                    }
                case Comp::Uint:
                    {
                        char* end = nullptr;
                        const unsigned long value = std::strtoul(token.c_str(), &end, 0);
                        if (end == token.c_str())
                            return false;
                        const uint32_t v = static_cast<uint32_t>(value);
                        std::memcpy(out, &v, sizeof(v));
                        return true;
                    }
                case Comp::Bool:
                    {
                        if (token != "true" && token != "false")
                            return false;
                        const int32_t v = token == "true" ? 1 : 0;
                        std::memcpy(out, &v, sizeof(v));
                        return true;
                    }
                }
                return false;
            };

            std::vector<uint8_t> bytes;
            bytes.reserve(count * 4);
            for (const auto& token : tokens)
            {
                uint8_t comp_bytes[4];
                if (!parse_token(token, comp_bytes))
                    return {};
                bytes.insert(bytes.end(), comp_bytes, comp_bytes + 4);
            }
            return bytes;
        }

        /* 从 GLSL 源码收集裸 uniform 的默认初始值（名字 → 字节序列）。
         * uniform block 成员不允许携带初始化器，因此不会误命中块内成员；
         * 采样器声明没有初始化器，同样不会命中。同名 uniform 以先出现者为准。 */
        void CollectDefaultUniformValues(const std::string& glsl_source,
            std::unordered_map<std::string, std::vector<uint8_t>>& out_values)
        {
            static const std::regex default_regex(
                R"((?:layout\s*\([^)]*\)\s*)?uniform\s+(?:highp\s+|mediump\s+|lowp\s+)?(bool|bvec[234]|int|ivec[234]|uint|uvec[234]|float|vec[234])\s+([A-Za-z_]\w*)\s*=\s*([^;]+)\s*;)");

            for (auto it = std::sregex_iterator(glsl_source.begin(), glsl_source.end(), default_regex);
                 it != std::sregex_iterator(); ++it)
            {
                const std::string type = (*it)[1].str();
                const std::string name = (*it)[2].str();
                const std::string init_expr = (*it)[3].str();

                auto bytes = SerializeUniformDefaultValue(type, init_expr);
                if (!bytes.empty())
                    out_values.emplace(name, std::move(bytes));
            }
        }

        [[nodiscard]] bool IsIntegerColorFormat(MTL::PixelFormat format)
        {
            switch (format)
            {
            case MTL::PixelFormatR8Sint:
            case MTL::PixelFormatR8Uint:
            case MTL::PixelFormatR16Sint:
            case MTL::PixelFormatR16Uint:
            case MTL::PixelFormatR32Sint:
            case MTL::PixelFormatR32Uint:
            case MTL::PixelFormatRG8Sint:
            case MTL::PixelFormatRG8Uint:
            case MTL::PixelFormatRG16Sint:
            case MTL::PixelFormatRG16Uint:
            case MTL::PixelFormatRG32Sint:
            case MTL::PixelFormatRG32Uint:
            case MTL::PixelFormatRGBA8Sint:
            case MTL::PixelFormatRGBA8Uint:
            case MTL::PixelFormatRGB10A2Uint:
            case MTL::PixelFormatRGBA16Sint:
            case MTL::PixelFormatRGBA16Uint:
            case MTL::PixelFormatRGBA32Sint:
            case MTL::PixelFormatRGBA32Uint:
                return true;
            default:
                return false;
            }
        }
    }

    uint64_t MetalPipelineDesc::Hash() const
    {
        uint64_t h = 0;
        for (MTL::PixelFormat format : ColorFormats)
            MetalHashCombine(h, static_cast<uint64_t>(format));
        MetalHashCombine(h, static_cast<uint64_t>(DepthFormat));
        MetalHashCombine(h, static_cast<uint64_t>(StencilFormat));
        MetalHashCombine(h, static_cast<uint64_t>(SampleCount));
        MetalHashCombine(h, static_cast<uint64_t>(VertexLayoutHash));

        /* 仅影响管线对象的状态参与哈希：混合与颜色写入掩码。
         * 剔除模式、正面朝向与深度状态由深度模板状态和编码器承担。 */
        MetalHashCombine(h, RasterState.EnableBlend ? 1u : 0u);
        MetalHashCombine(h, RasterState.EnableColorWrite ? 1u : 0u);
        if (RasterState.EnableBlend)
        {
            MetalHashCombine(h, static_cast<uint64_t>(RasterState.BlendEquationRGB));
            MetalHashCombine(h, static_cast<uint64_t>(RasterState.BlendEquationA));
            MetalHashCombine(h, static_cast<uint64_t>(RasterState.BlendFuncSrcRGB));
            MetalHashCombine(h, static_cast<uint64_t>(RasterState.BlendFuncSrcA));
            MetalHashCombine(h, static_cast<uint64_t>(RasterState.BlendFuncDstRGB));
            MetalHashCombine(h, static_cast<uint64_t>(RasterState.BlendFuncDstA));
        }
        return h;
    }

    MetalShader::MetalShader(const std::string& filepath)
        : DeviceShader(filepath)
    {
        PROFILE_FUNCTION();

        m_DebugName = ExtractFilename(filepath);

        const std::string source = ShaderCompiler::ReadFile(filepath);
        if (source.empty())
        {
            CORE_LOG_ERROR("Failed to read shader file: {}", filepath);
            return;
        }

        std::unordered_map<ShaderCompiler::ShaderStage, std::string> shader_sources;
        ShaderCompiler::PreProcessShaderSrc(source, shader_sources);
        if (shader_sources.find(ShaderCompiler::ShaderStage::Vertex) == shader_sources.end() ||
            shader_sources.find(ShaderCompiler::ShaderStage::Fragment) == shader_sources.end())
        {
            CORE_LOG_ERROR("Invalid shader format: missing #type vertex/#type fragment in {}", filepath);
            return;
        }

        CompileAndReflect(shader_sources[ShaderCompiler::ShaderStage::Vertex],
            shader_sources[ShaderCompiler::ShaderStage::Fragment]);
    }

    MetalShader::MetalShader(std::string name, const std::string& vertex_src, const std::string& pixel_src)
        : DeviceShader(""), m_DebugName(std::move(name))
    {
        PROFILE_FUNCTION();
        CompileAndReflect(vertex_src, pixel_src);
    }

    void MetalShader::CompileAndReflect(const std::string& vertex_src, const std::string& fragment_src)
    {
        m_VertexMSL = ShaderCompiler::Compile(vertex_src,
            ShaderCompiler::ShaderStage::Vertex, ShaderCompiler::ShaderTarget::Metal, m_DebugName);
        m_FragmentMSL = ShaderCompiler::Compile(fragment_src,
            ShaderCompiler::ShaderStage::Fragment, ShaderCompiler::ShaderTarget::Metal, m_DebugName);

        m_Reflection.Clear();
        ReflectFromGLSLSource(vertex_src);
        ReflectFromGLSLSource(fragment_src);

        /* 解析 GLSL 裸 uniform 的默认初始值：MSL 丢失源码默认值，
         * 创建材质块时需借此还原（详见 CollectDefaultUniformValues） */
        m_DefaultUniformValues.clear();
        CollectDefaultUniformValues(vertex_src, m_DefaultUniformValues);
        CollectDefaultUniformValues(fragment_src, m_DefaultUniformValues);

        /* 以 MSL 形参为准修正资源槽位：SPIRV-Cross 会把 GLSL 组合采样器拆成
         * texture 与 sampler 两个参数，其索引只有在生成后才能确定。 */
        ParseShaderResources(m_VertexMSL, true);
        ParseShaderResources(m_FragmentMSL, false);

        CORE_LOG_INFO("Metal shader created: {}", m_DebugName);
    }

    MetalShader::~MetalShader()
    {
        if (m_VertexLibrary)
            m_VertexLibrary->release();
        if (m_FragmentLibrary)
            m_FragmentLibrary->release();

        for (auto& [key, state] : m_PipelineStates)
        {
            if (state)
                state->release();
        }
        m_PipelineStates.clear();

        if (m_VertexDescriptor)
            m_VertexDescriptor->release();

        ReleaseMaterialBlocks();
    }

    void MetalShader::ReleaseMaterialBlocks()
    {
        for (auto& block : m_MaterialBlocks)
        {
            if (block.Buffer)
                block.Buffer->release();
        }
        m_MaterialBlocks.clear();
    }

    void MetalShader::Bind()
    {
        auto* render_api = dynamic_cast<MetalRenderAPI*>(Renderer::GetRenderAPI().get());
        if (!render_api)
            return;

        /* 这里只登记当前着色器。管线状态与深度模板状态都依赖光栅状态与当前
         * RenderPass 的附件格式，必须等到绘制时才能确定，否则会与随后的
         * ApplyRasterState 互相覆盖。 */
        render_api->SetBoundShader(this);
    }

    void MetalShader::Unbind()
    {
        /* Metal 后端无需显式解绑 */
    }

    void MetalShader::BindVertexArray(const SharedPtr<DeviceVertexArray>& vertex_array)
    {
        auto metal_vertex_array = std::dynamic_pointer_cast<MetalVertexArray>(vertex_array);
        if (metal_vertex_array)
            SetVertexDescriptor(metal_vertex_array->GetVertexDescriptor(),
                metal_vertex_array->GetVertexDescriptorHash());
    }

    void MetalShader::ParseShaderResources(const std::string& msl_source, bool is_vertex_stage)
    {
        if (msl_source.empty())
            return;

        ExtractMaterialBlocks(msl_source, is_vertex_stage);
        ExtractTextureAndSamplerBindings(msl_source);
    }

    void MetalShader::ExtractMaterialBlocks(const std::string& msl_source, bool is_vertex_stage)
    {
        static const std::regex struct_regex(R"(struct\s+(\w+)\s*\{([^}]*)\})");
        static const std::regex block_regex(R"(constant\s+(\w+)\s*&\s+\w+\s*\[\[buffer\((\d+)\)\]\])");
        static const std::regex member_regex(R"(([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*;)");

        std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> structs;
        for (auto it = std::sregex_iterator(msl_source.begin(), msl_source.end(), struct_regex);
             it != std::sregex_iterator(); ++it)
        {
            const std::string body = (*it)[2].str();
            std::vector<std::pair<std::string, std::string>> members;
            for (auto mit = std::sregex_iterator(body.begin(), body.end(), member_regex);
                 mit != std::sregex_iterator(); ++mit)
            {
                members.emplace_back((*mit)[1].str(), (*mit)[2].str());
            }
            structs[(*it)[1].str()] = std::move(members);
        }

        for (auto it = std::sregex_iterator(msl_source.begin(), msl_source.end(), block_regex);
             it != std::sregex_iterator(); ++it)
        {
            const std::string struct_name = (*it)[1].str();
            const uint32_t buffer_index = static_cast<uint32_t>(std::stoul((*it)[2].str()));

            /* 只有"默认 uniform 块"（非 block uniform 聚合出来的）才是着色器内嵌的材质参数。显式 binding
             * 的块归 DeviceUniformBuffer 管（View / Object / Material / Light / UI 等），重复提交会把
             * 调用方写进去的数据覆盖掉。 */
            if (!MetalBinding::IsDefaultUniformBufferIndex(buffer_index))
                continue;

            auto sit = structs.find(struct_name);
            if (sit == structs.end())
                continue;

            MaterialBlock block;
            block.BufferIndex = buffer_index;
            block.IsVertexStage = is_vertex_stage;

            uint32_t offset = 0;
            for (const auto& [type, member] : sit->second)
            {
                const uint32_t alignment = GetMaterialTypeAlignment(type);
                const uint32_t size = GetMaterialTypeSize(type);
                offset = (offset + alignment - 1u) / alignment * alignment;
                block.Offsets[member] = offset;
                offset += size;
            }

            if (block.Offsets.empty())
                continue;

            block.Size = (offset + 15u) / 16u * 16u;
            block.Shadow.assign(block.Size, 0);

            /* 用 GLSL 源码声明的默认初始值填充影子缓冲（未声明的成员保持零）：
             * MSL 的 constant 结构体成员不支持初始化器，若沿用全零，未通过
             * 材质写入的 uniform 会读到 0 而非源码默认值。 */
            for (const auto& [member, member_offset] : block.Offsets)
            {
                const auto dit = m_DefaultUniformValues.find(member);
                if (dit == m_DefaultUniformValues.end())
                    continue;

                const auto& default_bytes = dit->second;
                if (member_offset + default_bytes.size() > block.Size)
                    continue;

                std::memcpy(block.Shadow.data() + member_offset,
                    default_bytes.data(), default_bytes.size());
            }

            block.UseInlineBytes = block.Size <= MetalBinding::MaxInlineBytes;

            if (!block.UseInlineBytes)
            {
                if (MTL::Device* device = MetalRuntime::Device())
                {
                    block.Buffer = device->newBuffer(block.Size, MetalStorage::BufferOptions);
                    if (!block.Buffer)
                    {
                        CORE_LOG_WARN("Failed to allocate material buffer ({} bytes), using inline bytes",
                            block.Size);
                        block.UseInlineBytes = true;
                    }
                }
                else
                {
                    block.UseInlineBytes = true;
                }
            }

            m_MaterialBlocks.push_back(std::move(block));
        }
    }

    void MetalShader::ExtractTextureAndSamplerBindings(const std::string& msl_source)
    {
        for (auto it = std::sregex_iterator(msl_source.begin(), msl_source.end(), GetTextureRegex());
             it != std::sregex_iterator(); ++it)
        {
            m_Reflection.SamplerBindings[(*it)[1].str()] =
                static_cast<uint32_t>(std::stoul((*it)[2].str()));
        }

        const std::string suffix = kSamplerSuffix;
        for (auto it = std::sregex_iterator(msl_source.begin(), msl_source.end(), GetSamplerRegex());
             it != std::sregex_iterator(); ++it)
        {
            const std::string name = (*it)[1].str();
            const uint32_t index = static_cast<uint32_t>(std::stoul((*it)[2].str()));
            m_Reflection.SamplerStateBindings[name] = index;

            /* SPIRV-Cross 把 `sampler2D u_X` 生成为 `texture2d u_X` + `sampler u_XSmplr`，
             * 这里把分离出的采样器名映射回 GLSL 组合采样器名，使材质按原始名字
             * 查询槽位时同时得到纹理与采样器索引。 */
            if (name.size() > suffix.size() &&
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            {
                const std::string glsl_name = name.substr(0, name.size() - suffix.size());
                if (m_Reflection.SamplerStateBindings.find(glsl_name) ==
                    m_Reflection.SamplerStateBindings.end())
                {
                    m_Reflection.SamplerStateBindings[glsl_name] = index;
                }
            }
        }
    }

    MetalShader::MaterialBlock* MetalShader::FindMaterialMember(const std::string& name,
        uint32_t& out_offset)
    {
        for (auto& block : m_MaterialBlocks)
        {
            auto it = block.Offsets.find(name);
            if (it != block.Offsets.end())
            {
                out_offset = it->second;
                return &block;
            }
        }
        return nullptr;
    }

    void MetalShader::WriteMaterialData(const std::string& name, const void* data, uint32_t size)
    {
        /* 同一 uniform 可能同时被顶点与片元阶段使用：SPIRV-Cross 会为每个阶段
         * 各生成一个 constant 块，但二者的成员布局一致。此处写入所有包含该成员
         * 的块，避免只更新其中一个阶段而读到陈旧数据。 */
        bool found = false;
        for (auto& block : m_MaterialBlocks)
        {
            const auto it = block.Offsets.find(name);
            if (it == block.Offsets.end())
                continue;

            const uint32_t offset = it->second;
            if (offset + size > block.Size)
            {
                CORE_LOG_ERROR("Material uniform '{}' write out of range: {} + {} > {}",
                    name, offset, size, block.Size);
                continue;
            }

            memcpy(block.Shadow.data() + offset, data, size);
            found = true;
        }

        /* 没命中任何材质块成员时告警：通常是 .mtl 参数跟 Shader 脱节，值被悄悄丢掉。采样器名字走
         * SetInt 是正常路径（MSL 函数形参、不是块成员），要排除掉免得误报；按名字去重防刷屏。 */
        if (!found &&
            m_Reflection.SamplerBindings.find(name) == m_Reflection.SamplerBindings.end() &&
            m_WarnedMissingUniforms.insert(name).second)
        {
            CORE_LOG_WARN("Uniform '{}' not found in shader '{}': the value is ignored "
                "(no matching uniform declaration in the default uniform block).",
                name, m_DebugName);
        }
    }

    int MetalShader::GetUniformLocation(const std::string& name)
    {
        uint32_t offset = 0;
        return FindMaterialMember(name, offset) ? static_cast<int>(offset) : -1;
    }

    void MetalShader::SetInt(const std::string& name, int value)
    {
        WriteMaterialData(name, &value, sizeof(int));
    }

    void MetalShader::SetIntArray(const std::string& name, int* values, uint32_t count)
    {
        WriteMaterialData(name, values, sizeof(int) * count);
    }

    void MetalShader::SetFloat(const std::string& name, float value)
    {
        WriteMaterialData(name, &value, sizeof(float));
    }

    void MetalShader::SetFloat2(const std::string& name, const glm::vec2& value)
    {
        WriteMaterialData(name, &value, sizeof(glm::vec2));
    }

    void MetalShader::SetFloat3(const std::string& name, const glm::vec3& value)
    {
        WriteMaterialData(name, &value, sizeof(glm::vec3));
    }

    void MetalShader::SetFloat4(const std::string& name, const glm::vec4& value)
    {
        WriteMaterialData(name, &value, sizeof(glm::vec4));
    }

    void MetalShader::SetMat4(const std::string& name, const glm::mat4& value)
    {
        WriteMaterialData(name, &value, sizeof(glm::mat4));
    }

    void MetalShader::CommitMaterialUniforms(MTL::RenderCommandEncoder* encoder)
    {
        if (!encoder)
            return;

        for (auto& block : m_MaterialBlocks)
        {
            if (block.Size == 0 || block.Shadow.empty())
                continue;

            if (block.UseInlineBytes)
            {
                /* setBytes 会把数据拷贝进命令缓冲区，每次绘制拥有独立副本，
                 * 因此同一帧内的多次提交互不干扰，也无需等待 GPU 完成。 */
                if (block.IsVertexStage)
                    encoder->setVertexBytes(block.Shadow.data(), block.Size, block.BufferIndex);
                else
                    encoder->setFragmentBytes(block.Shadow.data(), block.Size, block.BufferIndex);
            }
            else
            {
                memcpy(block.Buffer->contents(), block.Shadow.data(), block.Size);
                block.Buffer->didModifyRange(NS::Range(0, block.Size));
                if (block.IsVertexStage)
                    encoder->setVertexBuffer(block.Buffer, 0, block.BufferIndex);
                else
                    encoder->setFragmentBuffer(block.Buffer, 0, block.BufferIndex);
            }
        }
    }

    void MetalShader::SetVertexDescriptor(MTL::VertexDescriptor* descriptor, uint64_t descriptor_hash)
    {
        if (m_VertexDescriptor == descriptor && m_VertexDescriptorHash == descriptor_hash)
            return;

        if (m_VertexDescriptor)
            m_VertexDescriptor->release();

        m_VertexDescriptor = descriptor;
        if (m_VertexDescriptor)
            m_VertexDescriptor->retain();

        /* 此处不清空管线缓存：MetalPipelineDesc::Hash 已包含 VertexLayoutHash，
         * 不同顶点布局的管线是不同的缓存条目，天然互不干扰。 */

        m_VertexDescriptorHash = descriptor_hash;
    }

    bool MetalShader::EnsureShaderLibraries()
    {
        if (m_VertexLibrary && m_FragmentLibrary)
            return true;

        MTL::Device* device = MetalRuntime::Device();
        if (!device)
        {
            CORE_LOG_ERROR("Metal device unavailable while creating shader: {}", m_DebugName);
            return false;
        }

        if (m_VertexMSL.empty() || m_FragmentMSL.empty())
        {
            CORE_LOG_ERROR("Shader source is empty for: {}", m_DebugName);
            return false;
        }

        NS::Error* error = nullptr;
        if (!m_VertexLibrary)
        {
            m_VertexLibrary = device->newLibrary(
                NS::String::string(m_VertexMSL.c_str(), NS::UTF8StringEncoding), nullptr, &error);
            if (!m_VertexLibrary)
            {
                METAL_CHECK_ERROR(error, "Failed to create vertex library");
                return false;
            }
        }

        if (!m_FragmentLibrary)
        {
            m_FragmentLibrary = device->newLibrary(
                NS::String::string(m_FragmentMSL.c_str(), NS::UTF8StringEncoding), nullptr, &error);
            if (!m_FragmentLibrary)
            {
                METAL_CHECK_ERROR(error, "Failed to create fragment library");
                return false;
            }
        }

        return true;
    }

    MTL::RenderPipelineState* MetalShader::GetPipelineState(const MetalPipelineDesc& desc)
    {
        const uint64_t key = desc.Hash();
        auto it = m_PipelineStates.find(key);
        if (it != m_PipelineStates.end())
            return it->second;

        return GetOrCreatePipelineState(desc);
    }

    MTL::RenderPipelineState* MetalShader::GetOrCreatePipelineState(const MetalPipelineDesc& desc)
    {
        MTL::Device* device = MetalRuntime::Device();
        if (!device || !EnsureShaderLibraries())
            return nullptr;

        MTL::Function* vertex_function = m_VertexLibrary->newFunction(
            NS::String::string("vertex_main", NS::UTF8StringEncoding));
        MTL::Function* fragment_function = m_FragmentLibrary->newFunction(
            NS::String::string("fragment_main", NS::UTF8StringEncoding));
        if (!vertex_function || !fragment_function)
        {
            CORE_LOG_ERROR("Failed to locate vertex_main/fragment_main in shader: {}", m_DebugName);
            if (vertex_function)
                vertex_function->release();
            if (fragment_function)
                fragment_function->release();
            return nullptr;
        }

        MTL::RenderPipelineDescriptor* descriptor = MTL::RenderPipelineDescriptor::alloc()->init();
        descriptor->setLabel(NS::String::string(m_DebugName.c_str(), NS::UTF8StringEncoding));
        descriptor->setVertexFunction(vertex_function);
        descriptor->setFragmentFunction(fragment_function);
        descriptor->setSampleCount(desc.SampleCount == 0 ? 1 : desc.SampleCount);

        /* 颜色附件格式全部来自当前 RenderPass，不假定任何固定格式，
         * 使 HDR、多渲染目标与 sRGB 输出都能正确创建管线。 */
        const size_t color_count = std::min<size_t>(desc.ColorFormats.size(), MAX_COLOR_ATTACHMENT_NUM);
        for (size_t i = 0; i < color_count; ++i)
        {
            MTL::RenderPipelineColorAttachmentDescriptor* attachment =
                descriptor->colorAttachments()->object(i);
            if (!attachment)
                continue;

            attachment->setPixelFormat(desc.ColorFormats[i]);
            attachment->setWriteMask(desc.RasterState.EnableColorWrite
                ? MTL::ColorWriteMaskAll : MTL::ColorWriteMaskNone);

            /* Metal 不支持整型颜色附件混合（如 GBuffer 的 R32Sint ObjectId），
             * 即使材质开启全局混合，也必须按附件格式单独关闭。 */
            const bool enable_blend = desc.RasterState.EnableBlend
                && !IsIntegerColorFormat(desc.ColorFormats[i]);
            attachment->setBlendingEnabled(enable_blend);
            if (enable_blend)
            {
                attachment->setRgbBlendOperation(
                    ToMetalBlendOperation(desc.RasterState.BlendEquationRGB));
                attachment->setAlphaBlendOperation(
                    ToMetalBlendOperation(desc.RasterState.BlendEquationA));
                attachment->setSourceRGBBlendFactor(
                    ToMetalBlendFactor(desc.RasterState.BlendFuncSrcRGB));
                attachment->setSourceAlphaBlendFactor(
                    ToMetalBlendFactor(desc.RasterState.BlendFuncSrcA));
                attachment->setDestinationRGBBlendFactor(
                    ToMetalBlendFactor(desc.RasterState.BlendFuncDstRGB));
                attachment->setDestinationAlphaBlendFactor(
                    ToMetalBlendFactor(desc.RasterState.BlendFuncDstA));
            }
        }

        descriptor->setDepthAttachmentPixelFormat(desc.DepthFormat);
        descriptor->setStencilAttachmentPixelFormat(desc.StencilFormat);

        if (m_VertexDescriptor)
            descriptor->setVertexDescriptor(m_VertexDescriptor);

        /* 接入管线编译产物缓存：先以「仅命中」模式创建——命中直接取用编译产物；
         * 未命中立即失败，回退为源码编译。 */
        MTL::BinaryArchive* pipeline_archive = MetalRuntime::PipelineArchive();
        if (pipeline_archive != nullptr)
            descriptor->setBinaryArchives(NS::Array::array(pipeline_archive));

        NS::Error* error = nullptr;
        MTL::RenderPipelineState* pipeline = nullptr;
        if (pipeline_archive != nullptr)
        {
            pipeline = device->newRenderPipelineState(
                descriptor, MTL::PipelineOptionFailOnBinaryArchiveMiss, nullptr, &error);
        }

        if (pipeline == nullptr)
        {
            /* 缓存未命中：从源码编译，并把真正新编译的条目写回缓存。
             * 只写回未命中条目——命中条目重复添加会在缓存中堆积冗余记录
             * （每条附带一份重命名的阶段产物），缓存文件会逐会话膨胀。 */
            error = nullptr;
            pipeline = device->newRenderPipelineState(descriptor, &error);

            if (pipeline != nullptr && pipeline_archive != nullptr)
                MetalRuntime::NotifyPipelineCompiled(descriptor);
        }

        descriptor->release();
        vertex_function->release();
        fragment_function->release();

        if (!pipeline)
        {
            CORE_LOG_ERROR("Failed to create pipeline state for '{}': {}", m_DebugName,
                error && error->localizedDescription()
                    ? error->localizedDescription()->utf8String() : "unknown");
            return nullptr;
        }

        m_PipelineStates[desc.Hash()] = pipeline;
        return pipeline;
    }
}

#endif /* PLATFORM_MACOS */
