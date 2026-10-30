#ifdef PLATFORM_MACOS

#include "Pch.h"
#include "MetalShader.h"
#include "MetalCommon.h"
#include "MetalRenderAPI.h"
#include "MetalUniformBuffer.h"
#include "MetalVertexArray.h"
#include "Helios/Renderer/Renderer.h"
#include "Helios/Renderer/ShaderCompiler.h"

#include <cstdlib>
#include <cstring>
#include <regex>

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

        /* 编码器上每个材质块槽位最后是哪一块写的（按阶段分开） */
        constexpr uint32_t kMaxBlockSlots = 32;
        struct BlockSlotOwner
        {
            const void* Block{ nullptr };
            uint32_t Generation{ 0 };
        };
        BlockSlotOwner g_BlockSlotOwners[2][kMaxBlockSlots];

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

        /* 不能参与混合的颜色格式：整型以外，32 位浮点也不支持。开启 blendingEnabled
         * 会让管线创建直接失败，表现为该材质的物体整批不画。 */
        [[nodiscard]] bool IsBlendUnsupportedFormat(MTL::PixelFormat format)
        {
            if (IsIntegerColorFormat(format))
                return true;

            switch (format)
            {
            case MTL::PixelFormatR32Float:
            case MTL::PixelFormatRG32Float:
            case MTL::PixelFormatRGBA32Float:
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

        ShaderSourceInfo source_info;
        auto shader_sources = ShaderCompiler::LoadStageSources(filepath, ShaderCompiler::ShaderTarget::Metal,
            {}, &source_info);
        m_SourceHash = std::move(source_info.Hash);
        m_RawHash = std::move(source_info.RawHash);
        m_Dependencies = std::move(source_info.Dependencies);
        if (shader_sources.empty())
        {
            CORE_LOG_ERROR("Failed to read shader file: {}", filepath);
            return;
        }

        /* 计算着色器：只有 #type compute 一段 */
        const auto compute_source = shader_sources.find(ShaderCompiler::ShaderStage::Compute);
        const auto vertex_source = shader_sources.find(ShaderCompiler::ShaderStage::Vertex);
        const auto fragment_source = shader_sources.find(ShaderCompiler::ShaderStage::Fragment);
        if (compute_source != shader_sources.end()
            && vertex_source == shader_sources.end() && fragment_source == shader_sources.end())
        {
            m_IsCompute = true;
            m_ComputeMSL = ShaderCompiler::Compile(compute_source->second,
                ShaderCompiler::ShaderStage::Compute, ShaderCompiler::ShaderTarget::Metal, m_DebugName);

            /* 线程组尺寸：读 local_size_x（缺省 64） */
            const std::string& glsl = compute_source->second;
            m_ThreadGroupSize = ParseThreadGroupSize(glsl);

            /* 反射 uniform 块 binding：派发时据此把缓冲放到 MSL 的高位槽位 */
            m_Reflection.Clear();
            ReflectFromGLSLSource(glsl);

            m_Loaded = !m_ComputeMSL.empty();
            if (!m_Loaded)
                CORE_LOG_ERROR("Compute shader compile failed: {}", m_DebugName);
            else
                CORE_LOG_INFO("Metal compute shader created: {}", m_DebugName);
            return;
        }

        if (vertex_source == shader_sources.end() || fragment_source == shader_sources.end())
        {
            CORE_LOG_ERROR("Invalid shader format: missing #type vertex/#type fragment in {}", filepath);
            return;
        }

        CompileAndReflect(vertex_source->second, fragment_source->second);
        m_Loaded = !m_VertexMSL.empty() && !m_FragmentMSL.empty();
        if (!m_Loaded)
            CORE_LOG_ERROR("Shader compile failed: {}", m_DebugName);
    }

    MetalShader::MetalShader(std::string name, const std::string& vertex_src, const std::string& pixel_src)
        : DeviceShader(""), m_DebugName(std::move(name))
    {
        PROFILE_FUNCTION();

        std::unordered_map<ShaderCompiler::ShaderStage, std::string> shader_sources{
            { ShaderCompiler::ShaderStage::Vertex, vertex_src },
            { ShaderCompiler::ShaderStage::Fragment, pixel_src } };
        ShaderCompiler::PrepareStageSources(shader_sources, ShaderCompiler::ShaderTarget::Metal, m_DebugName);
        CompileAndReflect(shader_sources[ShaderCompiler::ShaderStage::Vertex],
            shader_sources[ShaderCompiler::ShaderStage::Fragment]);
        m_Loaded = !m_VertexMSL.empty() && !m_FragmentMSL.empty();
    }

    void MetalShader::CompileAndReflect(const std::string& vertex_src, const std::string& fragment_src)
    {
        m_VertexMSL = ShaderCompiler::Compile(vertex_src,
            ShaderCompiler::ShaderStage::Vertex, ShaderCompiler::ShaderTarget::Metal, m_DebugName);
        m_FragmentMSL = ShaderCompiler::Compile(fragment_src,
            ShaderCompiler::ShaderStage::Fragment, ShaderCompiler::ShaderTarget::Metal, m_DebugName);

        m_Reflection.Clear();
        m_TextureStageMasks.clear();
        ReflectFromGLSLSource(vertex_src);
        ReflectFromGLSLSource(fragment_src);
        ReflectVertexAttributes(vertex_src);

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

    bool MetalShader::ApplyPrepared(DeviceShader& prepared)
    {
        PROFILE_FUNCTION();

        auto* fresh = dynamic_cast<MetalShader*>(&prepared);
        if (fresh == nullptr || !fresh->IsValid())
        {
            CORE_LOG_ERROR("Shader reload failed, keeping the previous version: {}", m_DebugName);
            return false;
        }

        /* 图形与计算互换要重建实例（渲染通道对它的用法不同），不在这里处理 */
        if (fresh->m_IsCompute != m_IsCompute)
        {
            CORE_LOG_ERROR("Shader '{}' switched between graphics and compute, reload skipped", m_DebugName);
            return false;
        }

        /* 只是碰了文件、内容没变（改注释另说）：什么都不用做 */
        if (!m_SourceHash.empty() && fresh->m_SourceHash == m_SourceHash)
        {
            /* 展开结果一样但 include 的增删可能不同，依赖表与原始指纹照样跟上 */
            m_Dependencies = std::move(fresh->m_Dependencies);
            m_RawHash = std::move(fresh->m_RawHash);
            return true;
        }

        /* 顶点属性位置没变时，按外部顶点数组建好的描述符继续可用（ImGui 这类只绑一次布局的也不受影响） */
        const bool same_vertex_layout =
            fresh->m_Reflection.VertexAttributeLocations == m_Reflection.VertexAttributeLocations;

        /* 材质块里的值按名字搬到新布局，改一行 shader 不该让材质参数错位或清零 */
        const auto values = SnapshotMaterialValues();
        StealFrom(*fresh);
        RestoreMaterialValues(values);

        /* 位置变了旧描述符作废：换给 fresh 随它释放，下次 BindVertexArray 按新反射重建 */
        if (!same_vertex_layout)
        {
            std::swap(m_VertexDescriptor, fresh->m_VertexDescriptor);
            std::swap(m_VertexDescriptorHash, fresh->m_VertexDescriptorHash);
            std::swap(m_VertexDescriptors, fresh->m_VertexDescriptors);
        }

        /* fresh 现在拿着旧库与旧管线，随调用方销毁回收 */
        ++m_Generation;
        return true;
    }

    bool MetalShader::BuildDeviceObjects()
    {
        PROFILE_FUNCTION();

        if (!m_Loaded)
            return false;

        /* 可能跑在后台线程：metal-cpp 的临时字符串要有自动释放池接着 */
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        const bool built = m_IsCompute ? GetComputePipelineState() != nullptr : EnsureShaderLibraries();
        pool->release();

        if (!built)
        {
            CORE_LOG_ERROR("Metal failed to build shader objects: {}", m_DebugName);
            m_Loaded = false;
        }
        return built;
    }

    void MetalShader::StealFrom(MetalShader& other)
    {
        std::swap(m_DebugName, other.m_DebugName);
        std::swap(m_VertexMSL, other.m_VertexMSL);
        std::swap(m_FragmentMSL, other.m_FragmentMSL);
        std::swap(m_VertexLibrary, other.m_VertexLibrary);
        std::swap(m_FragmentLibrary, other.m_FragmentLibrary);
        std::swap(m_PipelineStates, other.m_PipelineStates);
        std::swap(m_FailedPipelines, other.m_FailedPipelines);
        std::swap(m_MaterialBlocks, other.m_MaterialBlocks);
        std::swap(m_ParamTargets, other.m_ParamTargets);
        std::swap(m_ParamHandles, other.m_ParamHandles);
        std::swap(m_TextureStageMasks, other.m_TextureStageMasks);
        std::swap(m_DefaultUniformValues, other.m_DefaultUniformValues);
        std::swap(m_WarnedMissingUniforms, other.m_WarnedMissingUniforms);
        std::swap(m_IsCompute, other.m_IsCompute);
        std::swap(m_ThreadGroupSize, other.m_ThreadGroupSize);
        std::swap(m_ComputeMSL, other.m_ComputeMSL);
        std::swap(m_ComputeLibrary, other.m_ComputeLibrary);
        std::swap(m_ComputePipeline, other.m_ComputePipeline);
        std::swap(m_Reflection, other.m_Reflection);
        std::swap(m_SourceHash, other.m_SourceHash);
        std::swap(m_RawHash, other.m_RawHash);
        std::swap(m_Dependencies, other.m_Dependencies);
        std::swap(m_Loaded, other.m_Loaded);
    }

    std::unordered_map<std::string, MetalShader::SavedMaterialValue> MetalShader::SnapshotMaterialValues() const
    {
        std::unordered_map<std::string, SavedMaterialValue> values;

        for (const MaterialBlock& block : m_MaterialBlocks)
        {
            for (const auto& [name, offset] : block.Offsets)
            {
                const auto type_it = block.Types.find(name);
                if (type_it == block.Types.end())
                    continue;

                const uint32_t size = GetMaterialTypeSize(type_it->second);
                if (offset + size > block.Shadow.size())
                    continue;

                /* 同名成员在顶点与片元各有一份，值一样，取先到的 */
                if (values.find(name) != values.end())
                    continue;

                SavedMaterialValue value;
                value.Type = type_it->second;
                value.Bytes.assign(block.Shadow.begin() + offset, block.Shadow.begin() + offset + size);
                values.emplace(name, std::move(value));
            }
        }
        return values;
    }

    void MetalShader::RestoreMaterialValues(const std::unordered_map<std::string, SavedMaterialValue>& values)
    {
        for (MaterialBlock& block : m_MaterialBlocks)
        {
            for (const auto& [name, offset] : block.Offsets)
            {
                const auto value_it = values.find(name);
                if (value_it == values.end())
                    continue;

                /* 搬迁口径：同名同类型才带回。类型变了当新参数处理，留源码默认值，
                 * 不把旧字节塞进新布局（脚本那边同样是名字加类型对得上才回灌） */
                const auto type_it = block.Types.find(name);
                if (type_it == block.Types.end() || type_it->second != value_it->second.Type)
                    continue;

                const uint32_t size = GetMaterialTypeSize(type_it->second);
                if (size != value_it->second.Bytes.size() || offset + size > block.Shadow.size())
                    continue;

                std::memcpy(block.Shadow.data() + offset, value_it->second.Bytes.data(), size);
            }

            /* 内容变了，下一帧无条件重传 */
            block.Dirty = true;
            block.CommittedGeneration = 0;
        }
    }

    MTL::ComputePipelineState* MetalShader::GetComputePipelineState()
    {
        if (!m_IsCompute || m_ComputeMSL.empty())
            return nullptr;

        if (m_ComputePipeline != nullptr)
            return m_ComputePipeline;

        MTL::Device* device = MetalRuntime::Device();
        if (device == nullptr)
            return nullptr;

        if (m_ComputeLibrary == nullptr)
        {
            NS::Error* error = nullptr;
            m_ComputeLibrary = device->newLibrary(
                NS::String::string(m_ComputeMSL.c_str(), NS::UTF8StringEncoding), nullptr, &error);
            if (m_ComputeLibrary == nullptr)
            {
                METAL_CHECK_ERROR(error, "Failed to create compute library");
                return nullptr;
            }
        }

        MTL::Function* function = m_ComputeLibrary->newFunction(
            NS::String::string("compute_main", NS::UTF8StringEncoding));
        if (function == nullptr)
        {
            CORE_LOG_ERROR("Failed to locate compute_main in shader: {}", m_DebugName);
            return nullptr;
        }

        NS::Error* pipeline_error = nullptr;
        m_ComputePipeline = device->newComputePipelineState(function, &pipeline_error);
        if (m_ComputePipeline == nullptr)
            METAL_CHECK_ERROR(pipeline_error, "Failed to create compute pipeline state");
        function->release();
        if (m_ComputePipeline == nullptr)
            CORE_LOG_ERROR("Failed to create compute pipeline state: {}", m_DebugName);

        return m_ComputePipeline;
    }

    MetalShader::~MetalShader()
    {
        if (m_VertexLibrary)
            m_VertexLibrary->release();
        if (m_FragmentLibrary)
            m_FragmentLibrary->release();

        /* 计算着色器独立一套库与管线（重载时旧对象随本对象析构回收） */
        if (m_ComputeLibrary)
            m_ComputeLibrary->release();
        if (m_ComputePipeline)
            m_ComputePipeline->release();

        for (auto& [key, state] : m_PipelineStates)
        {
            if (state)
                state->release();
        }
        m_PipelineStates.clear();

        if (m_VertexDescriptor)
        {
            m_VertexDescriptor->release();
            m_VertexDescriptor = nullptr;
        }

        for (auto& [layout_hash, descriptor] : m_VertexDescriptors)
        {
            if (descriptor)
                descriptor->release();
        }
        m_VertexDescriptors.clear();

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
        /* Metal 后端的 RenderAPI 一定是 MetalRenderAPI，每次绘制都走这里，不做 RTTI 转换 */
        auto* render_api = static_cast<MetalRenderAPI*>(Renderer::GetRenderAPI().get());
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
        BindVertexArray(vertex_array, VertexBufferLayout{});
    }

    void MetalShader::BindVertexArray(const SharedPtr<DeviceVertexArray>& vertex_array,
        const VertexBufferLayout& instance_layout)
    {
        auto* metal_vertex_array = static_cast<MetalVertexArray*>(vertex_array.get());
        if (!metal_vertex_array)
            return;

        const bool instanced = !instance_layout.GetElements().empty();

        /* 顶点布局键：带实例属性的和不带的不是同一条管线 */
        uint64_t layout_hash = metal_vertex_array->GetVertexDescriptorHash();
        if (instanced)
        {
            MetalHashCombine(layout_hash, 0x1ULL);
            MetalHashCombine(layout_hash, static_cast<uint64_t>(instance_layout.GetStride()));
            for (const BufferElement& element : instance_layout.GetElements())
            {
                MetalHashCombine(layout_hash, static_cast<uint64_t>(element.Type));
                MetalHashCombine(layout_hash, static_cast<uint64_t>(element.Offset));
                MetalHashCombine(layout_hash, static_cast<uint64_t>(element.GetComponentCount()));
            }
        }
        m_ActiveVertexLayoutHash = layout_hash;

        auto it = m_VertexDescriptors.find(layout_hash);
        if (it == m_VertexDescriptors.end())
        {
            MTL::VertexDescriptor* descriptor = BuildVertexDescriptor(*metal_vertex_array,
                instanced ? &instance_layout : nullptr);
            if (descriptor == nullptr)
            {
                /* 布局和着色器对不上：这条绘制不成立，错误在 BuildVertexDescriptor 里已写明 */
                return;
            }

            it = m_VertexDescriptors.emplace(layout_hash, descriptor).first;
        }

        SetVertexDescriptor(it->second, layout_hash);
    }

    MTL::VertexDescriptor* MetalShader::BuildVertexDescriptor(const MetalVertexArray& vertex_array,
        const VertexBufferLayout* instance_layout)
    {
        MTL::VertexDescriptor* descriptor = MTL::VertexDescriptor::alloc()->init();
        if (descriptor == nullptr)
            return nullptr;

        const auto& buffers = vertex_array.GetVertexBuffers();
        std::unordered_map<uint32_t, std::string> filled;
        /* 被属性引用到的缓冲槽：没被引用的槽不能设 stride，Metal 会判为配置错误 */
        std::unordered_map<uint32_t, const VertexBufferLayout*> used_layouts;

        /* 内嵌 MSL 的着色器（如 ImGui）没有 GLSL 声明可反射，退回按出现顺序分配 */
        const bool named = !m_Reflection.VertexAttributeLocations.empty();
        if (!named)
            CORE_LOG_WARN("Shader '{}' has no vertex attribute reflection, falling back to element order", m_DebugName);

        uint32_t sequential_location = 0;

        for (size_t buffer_index = 0; buffer_index < buffers.size(); ++buffer_index)
        {
            const SharedPtr<DeviceVertexBuffer>& buffer = buffers[buffer_index];
            if (buffer == nullptr)
                continue;

            const VertexBufferLayout& layout = buffer->GetLayout();

            for (const BufferElement& element : layout.GetElements())
            {
                uint32_t location = sequential_location;
                if (named)
                {
                    const int reflected = m_Reflection.FindVertexAttributeLocation(element.Name);

                    /* 布局里多出来的属性（着色器不读）直接忽略 */
                    if (reflected < 0)
                        continue;
                    location = static_cast<uint32_t>(reflected);
                }
                else
                {
                    ++sequential_location;
                }

                const auto inserted = filled.emplace(location, element.Name);
                if (!inserted.second)
                {
                    CORE_LOG_ERROR("Shader '{}' gets two attributes for location {}: '{}' and '{}'",
                        m_DebugName, location, inserted.first->second, element.Name);
                    continue;
                }

                MTL::VertexAttributeDescriptor* attribute =
                    descriptor->attributes()->object(static_cast<NS::UInteger>(location));
                attribute->setFormat(ToMetalVertexFormat(element.Type));
                attribute->setBufferIndex(static_cast<NS::UInteger>(MetalBinding::VertexBufferBase + buffer_index));
                attribute->setOffset(element.Offset);

                used_layouts[static_cast<uint32_t>(buffer_index)] = &layout;
            }
        }

        for (const auto& [buffer_index, layout] : used_layouts)
        {
            MTL::VertexBufferLayoutDescriptor* layout_descriptor =
                descriptor->layouts()->object(static_cast<NS::UInteger>(buffer_index));
            layout_descriptor->setStride(layout->GetStride());
            layout_descriptor->setStepFunction(MTL::VertexStepFunctionPerVertex);
        }

        /* 实例属性：接在顶点缓冲区后面占一个槽位，按实例步进；属性名与着色器声明对齐 */
        if (instance_layout != nullptr && !instance_layout->GetElements().empty())
        {
            const uint32_t instance_buffer_index = static_cast<uint32_t>(buffers.size());
            const VertexBufferLayout& layout = *instance_layout;

            /* 顶点缓冲已占满 16 个槽时再往后放就压到 uniform 槽上了 */
            if (instance_buffer_index >= MetalBinding::MaxVertexBuffers)
            {
                CORE_LOG_ERROR("Shader '{}': mesh uses all {} vertex buffer slots, no room for instance data",
                    m_DebugName, MetalBinding::MaxVertexBuffers);
                descriptor->release();
                return nullptr;
            }

            uint32_t used_count = 0;
            for (const BufferElement& element : layout.GetElements())
            {
                const int reflected = named ? m_Reflection.FindVertexAttributeLocation(element.Name) : -1;
                if (reflected < 0)
                    continue;

                MTL::VertexAttributeDescriptor* attribute =
                    descriptor->attributes()->object(static_cast<NS::UInteger>(reflected));
                attribute->setFormat(ToMetalVertexFormat(element.Type));
                attribute->setBufferIndex(static_cast<NS::UInteger>(MetalBinding::VertexBufferBase + instance_buffer_index));
                attribute->setOffset(element.Offset);
                filled.emplace(static_cast<uint32_t>(reflected), element.Name);
                ++used_count;
            }

            if (used_count > 0)
            {
                MTL::VertexBufferLayoutDescriptor* layout_descriptor =
                    descriptor->layouts()->object(static_cast<NS::UInteger>(instance_buffer_index));
                layout_descriptor->setStride(layout.GetStride());
                layout_descriptor->setStepFunction(MTL::VertexStepFunctionPerInstance);
                layout_descriptor->setStepRate(1);
            }
        }

        /* 着色器声明了、布局给不出：管线建不出来，这里说清楚缺哪个 */
        for (const auto& [name, location] : m_Reflection.VertexAttributeLocations)
        {
            if (filled.find(location) != filled.end())
                continue;

            CORE_LOG_ERROR("Shader '{}' declares '{}' at location {}, but the vertex layout has no such attribute",
                m_DebugName, name, location);
            descriptor->release();
            return nullptr;
        }

        return descriptor;
    }

    void MetalShader::ParseShaderResources(const std::string& msl_source, bool is_vertex_stage)
    {
        if (msl_source.empty())
            return;

        ExtractMaterialBlocks(msl_source, is_vertex_stage);
        ExtractTextureAndSamplerBindings(msl_source, is_vertex_stage);
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
                block.Types[member] = type;
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

    void MetalShader::ExtractTextureAndSamplerBindings(const std::string& msl_source, bool is_vertex_stage)
    {
        const uint8_t stage = is_vertex_stage
            ? ShaderReflectionData::StageVertex : ShaderReflectionData::StageFragment;

        for (auto it = std::sregex_iterator(msl_source.begin(), msl_source.end(), GetTextureRegex());
             it != std::sregex_iterator(); ++it)
        {
            const std::string name = (*it)[1].str();
            m_Reflection.SamplerBindings[name] = static_cast<uint32_t>(std::stoul((*it)[2].str()));
            m_Reflection.SamplerStages[name] |= stage;
        }

        const std::string suffix = kSamplerSuffix;
        for (auto it = std::sregex_iterator(msl_source.begin(), msl_source.end(), GetSamplerRegex());
             it != std::sregex_iterator(); ++it)
        {
            const std::string name = (*it)[1].str();
            const uint32_t index = static_cast<uint32_t>(std::stoul((*it)[2].str()));
            m_Reflection.SamplerStateBindings[name] = index;
            m_Reflection.SamplerStages[name] |= stage;

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
                m_Reflection.SamplerStages[glsl_name] |= stage;
            }
        }
    }

    uint8_t MetalShader::GetTextureStageMask(uint32_t slot)
    {
        const auto cached = m_TextureStageMasks.find(slot);
        if (cached != m_TextureStageMasks.end())
            return cached->second;

        uint8_t mask = ShaderReflectionData::StageAll;
        for (const auto& [name, binding] : m_Reflection.SamplerBindings)
        {
            if (binding != slot)
                continue;

            mask = m_Reflection.FindSamplerStageMask(name);
            break;
        }

        m_TextureStageMasks[slot] = mask;
        return mask;
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

            if (!WriteBlockBytes(block, it->second, data, size))
            {
                CORE_LOG_ERROR("Material uniform '{}' write out of range: {} + {} > {}",
                    name, it->second, size, block.Size);
                continue;
            }
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

    bool MetalShader::WriteBlockBytes(MaterialBlock& block, uint32_t offset, const void* data, uint32_t size)
    {
        if (offset + size > block.Size || offset + size > block.Shadow.size())
            return false;

        uint8_t* target = block.Shadow.data() + offset;
        if (std::memcmp(target, data, size) != 0)
        {
            std::memcpy(target, data, size);
            block.Dirty = true;
        }
        return true;
    }

    int MetalShader::ResolveParamHandle(const std::string& name)
    {
        if (const auto cached = m_ParamHandles.find(name); cached != m_ParamHandles.end())
            return cached->second;

        /* 同名成员顶点与片元各一份，两块都要写 */
        ParamTarget target;
        for (uint32_t index = 0; index < m_MaterialBlocks.size() && target.Count < 2; ++index)
        {
            const auto it = m_MaterialBlocks[index].Offsets.find(name);
            if (it == m_MaterialBlocks[index].Offsets.end())
                continue;
            target.Block[target.Count] = index;
            target.Offset[target.Count] = it->second;
            ++target.Count;
        }

        int handle = -1;
        if (target.Count > 0)
        {
            handle = static_cast<int>(m_ParamTargets.size());
            m_ParamTargets.push_back(target);
        }
        else if (m_Reflection.SamplerBindings.find(name) == m_Reflection.SamplerBindings.end()
            && m_WarnedMissingUniforms.insert(name).second)
        {
            /* 与按名字写的告警同口径：采样器不是块成员，不算缺失 */
            CORE_LOG_WARN("Uniform '{}' not found in shader '{}': the value is ignored "
                "(no matching uniform declaration in the default uniform block).",
                name, m_DebugName);
        }

        m_ParamHandles.emplace(name, handle);
        return handle;
    }

    void MetalShader::SetParam(int handle, ParamType type, const void* data)
    {
        if (handle < 0 || data == nullptr || static_cast<size_t>(handle) >= m_ParamTargets.size())
            return;

        uint32_t size = 0;
        switch (type)
        {
        case ParamType::Int: size = sizeof(int); break;
        case ParamType::Float: size = sizeof(float); break;
        case ParamType::Vec2: size = sizeof(glm::vec2); break;
        case ParamType::Vec3: size = sizeof(glm::vec3); break;
        case ParamType::Vec4: size = sizeof(glm::vec4); break;
        case ParamType::Mat4: size = sizeof(glm::mat4); break;
        case ParamType::Texture: return;
        }

        const ParamTarget& target = m_ParamTargets[handle];
        for (uint32_t index = 0; index < target.Count; ++index)
        {
            MaterialBlock& block = m_MaterialBlocks[target.Block[index]];
            if (!WriteBlockBytes(block, target.Offset[index], data, size))
                CORE_LOG_ERROR("Material uniform write out of range in shader '{}': {} + {} > {}",
                    m_DebugName, target.Offset[index], size, block.Size);
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

        const uint32_t generation = MetalRuntime::EncoderGeneration();
        for (auto& block : m_MaterialBlocks)
        {
            if (block.Size == 0 || block.Shadow.empty())
                continue;

            /* 值没变、同一编码器里这个槽位最后也是自己写的，就不必再传；
             * 别的着色器在同一槽位传过自己的块，得重新传一遍 */
            BlockSlotOwner& owner = g_BlockSlotOwners[block.IsVertexStage ? 0 : 1]
                [std::min<uint32_t>(block.BufferIndex, kMaxBlockSlots - 1)];
            if (!block.Dirty && block.CommittedGeneration == generation
                && owner.Block == &block && owner.Generation == generation)
                continue;

            /* 优先写进环形缓冲：不占命令缓冲带宽，绑定只带偏移 */
            uint32_t ring_offset = 0;
            if (MetalUniformRing::Instance().Allocate(block.Size, ring_offset))
            {
                MTL::Buffer* ring_buffer = MetalUniformRing::Instance().GetBuffer();
                memcpy(static_cast<uint8_t*>(ring_buffer->contents()) + ring_offset,
                    block.Shadow.data(), block.Size);

                if (block.IsVertexStage)
                    encoder->setVertexBuffer(ring_buffer, ring_offset, block.BufferIndex);
                else
                    encoder->setFragmentBuffer(ring_buffer, ring_offset, block.BufferIndex);
            }
            else if (block.UseInlineBytes)
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

            block.Dirty = false;
            block.CommittedGeneration = generation;
            owner.Block = &block;
            owner.Generation = generation;
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

        if (m_FailedPipelines.count(key) != 0)
            return nullptr;

        MTL::RenderPipelineState* pipeline = GetOrCreatePipelineState(desc);
        /* 缺顶点描述符导致的失败不记：补上 BindVertexArray 后同一组参数要能重建 */
        const bool missing_descriptor = m_VertexDescriptor == nullptr && !m_Reflection.VertexAttributeLocations.empty();
        if (pipeline == nullptr && !missing_descriptor)
            m_FailedPipelines.insert(key);
        return pipeline;
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

            /* Metal 的整型与 32 位浮点颜色附件都不支持混合（如 ObjectId 这类附加通道），
             * 即使材质开启全局混合，也要按附件格式单独关掉。 */
            const bool enable_blend = desc.RasterState.EnableBlend
                && !IsBlendUnsupportedFormat(desc.ColorFormats[i]);
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

            /* 热重载出来的版本不进缓存：编辑期每改一版就多一批条目，缓存文件只会越积越大 */
            if (pipeline != nullptr && pipeline_archive != nullptr && m_Generation == 1)
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
