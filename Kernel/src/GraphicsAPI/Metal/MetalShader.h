#pragma once
#include "Platform/Platform.h"

#if HELIOS_PLATFORM_APPLE

#include "Helios/VirtualDevice/DeviceShader.h"
#include "Helios/Renderer/RenderCommon.h"
#include <Metal/Metal.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Helios
{
    class MetalVertexArray;

    /* 创建管线状态所需的全部外部信息。
     * 由 MetalRenderAPI 在绘制时依据当前 RenderPass 的附件格式、采样数与
     * 光栅状态填充。 */
    struct MetalPipelineDesc
    {
        /* 颜色附件格式，索引即 attachment 位置；空表示仅有深度/模板附件 */
        std::vector<MTL::PixelFormat> ColorFormats;
        MTL::PixelFormat DepthFormat{ MTL::PixelFormatInvalid };
        MTL::PixelFormat StencilFormat{ MTL::PixelFormatInvalid };
        uint32_t SampleCount{ 1 };
        RenderRasterState RasterState{};
        uint64_t VertexLayoutHash{ 0 };

        [[nodiscard]] uint64_t Hash() const;
    };

    class MetalShader : public DeviceShader
    {
    public:
        MetalShader(const std::string& filepath, const ShaderVariantDefines& defines = {});
        MetalShader(std::string name, const std::string& vertex_src, const std::string& pixel_src);
        ~MetalShader() override;

        void Bind() override;
        void Unbind() override;

        void BindVertexArray(const SharedPtr<DeviceVertexArray>& vertex_array) override;
        /* 带实例布局的绑定：实例属性进顶点描述符，管线键把实例布局也算进去 */
        void BindVertexArray(const SharedPtr<DeviceVertexArray>& vertex_array,
            const VertexBufferLayout& instance_layout) override;
        /* 最近一次绑定用的顶点布局键（含实例布局）：绘制时按它取管线 */
        [[nodiscard]] uint64_t GetActiveVertexLayoutHash() const { return m_ActiveVertexLayoutHash; }

        int GetUniformLocation(const std::string& name) override;
        void SetInt(const std::string& name, int value) override;
        void SetIntArray(const std::string& name, int* values, uint32_t count) override;
        void SetFloat(const std::string& name, float value) override;
        void SetFloat2(const std::string& name, const glm::vec2& value) override;
        void SetFloat3(const std::string& name, const glm::vec3& value) override;
        void SetFloat4(const std::string& name, const glm::vec4& value) override;
        void SetMat4(const std::string& name, const glm::mat4& value) override;
        int ResolveParamHandle(const std::string& name) override;
        void SetParam(int handle, ParamType type, const void* data) override;

        const std::string& GetDebugName() const override { return m_DebugName; }

        /* 依据当前渲染目标与光栅状态获取管线状态，未命中缓存时惰性创建 */
        MTL::RenderPipelineState* GetPipelineState(const MetalPipelineDesc& desc);

        MTL::VertexDescriptor* GetVertexDescriptor() const { return m_VertexDescriptor; }

        /* 设置顶点布局。descriptor_hash 由 MetalVertexArray 计算，既用于判断
         * 布局是否变化，也作为管线缓存键的一部分。 */
        void SetVertexDescriptor(MTL::VertexDescriptor* descriptor, uint64_t descriptorHash);

        /* 把材质参数（着色器内嵌的非 block uniform）提交给编码器：setVertexBytes / setFragmentBytes
         * 让每次绘制都有独立的数据副本，消除同帧多次写同一缓冲区的竞争；超过内联上限就自动回退到
         * 独立缓冲区。 */
        void CommitMaterialUniforms(MTL::RenderCommandEncoder* encoder);

        /* 这张贴图被哪些阶段采样（按槽位反查，结果缓存）。查不到时按两个阶段都绑。 */
        uint8_t GetTextureStageMask(uint32_t slot);

        /* 计算着色器（#type compute）：惰性建管线，线程组尺寸从 local_size_x 读 */
        MTL::ComputePipelineState* GetComputePipelineState();
        [[nodiscard]] bool IsCompute() const override { return m_IsCompute; }
        [[nodiscard]] uint32_t GetThreadGroupSize() const override { return m_ThreadGroupSize; }

        /* 换入另一份实例的编译产物：失败保持原样，成功后代价只有交换本身 */
        bool ApplyPreparedImpl(DeviceShader& prepared) override;

        /* 后台把 library 与计算管线建好，Metal 编不过 MSL 时在换入前就判失败 */
        bool BuildDeviceObjects() override;

        /* 构造是否真的拿到了可用的编译产物（语法错误时为空） */
        [[nodiscard]] bool IsValid() const { return m_Loaded; }

        /* 这组管线参数之前建失败过：失败只报一次，也不每次绘制都重试 */
        [[nodiscard]] bool HasPipelineFailed(uint64_t key) const { return m_FailedPipelines.count(key) != 0; }

    private:
        /* 计算着色器状态 */
        bool m_IsCompute{ false };
        uint32_t m_ThreadGroupSize{ 64 };
        std::string m_ComputeMSL;
        MTL::Library* m_ComputeLibrary{ nullptr };
        MTL::ComputePipelineState* m_ComputePipeline{ nullptr };

    public:

        /* 着色器内嵌的一块材质参数（由 SPIRV-Cross 生成为单个 constant buffer） */
        struct MaterialBlock
        {
            uint32_t BufferIndex{ 0 };                        /* MSL buffer 索引 */
            uint32_t Size{ 0 };                               /* 数据大小（字节） */
            bool IsVertexStage{ false };                      /* 绑定的着色阶段 */
            bool UseInlineBytes{ true };                      /* 是否走 setBytes 快速路径 */
            MTL::Buffer* Buffer{ nullptr };                   /* 大块数据的后备缓冲区 */
            std::unordered_map<std::string, uint32_t> Offsets; /* 成员名 → 字节偏移 */
            /* 成员名 → MSL 类型：重载迁移时按它判断类型变没变，大小也由它算 */
            std::unordered_map<std::string, std::string> Types;
            std::vector<uint8_t> Shadow;                      /* CPU 端影子数据 */
            /* 影子数据改过没提交：值没变的写入不置位，同一编码器里就不必再传 */
            bool Dirty{ true };
            uint32_t CommittedGeneration{ 0 };                /* 上次提交的编码器世代号 */
        };

        /* 编译并反射着色器：生成 MSL、提取材质参数布局与资源槽位 */
        void CompileAndReflect(const std::string& vertex_src, const std::string& fragment_src);

        /* 从 MSL 中解析材质参数块与纹理/采样器槽位 */
        void ParseShaderResources(const std::string& msl_source, bool is_vertex_stage);
        void ExtractMaterialBlocks(const std::string& msl_source, bool is_vertex_stage);
        void ExtractTextureAndSamplerBindings(const std::string& msl_source, bool is_vertex_stage);

        bool EnsureShaderLibraries();
        MTL::RenderPipelineState* GetOrCreatePipelineState(const MetalPipelineDesc& desc);
        void ReleaseMaterialBlocks();

        /* 按着色器声明的属性位置把顶点布局映射成 Metal 描述符：布局里多出来的元素忽略，
         * 着色器要而布局里没有的报错并返回空。每种布局的结果缓存复用。 */
        MTL::VertexDescriptor* BuildVertexDescriptor(const MetalVertexArray& vertex_array,
            const VertexBufferLayout* instance_layout);

        /* 在全部材质块中查找成员，命中时输出所在块与偏移 */
        MaterialBlock* FindMaterialMember(const std::string& name, uint32_t& out_offset);
        void WriteMaterialData(const std::string& name, const void* data, uint32_t size);
        /* 写进一块的影子数据，越界返回 false；值有变化才标脏 */
        bool WriteBlockBytes(MaterialBlock& block, uint32_t offset, const void* data, uint32_t size);

        std::string m_DebugName{ "Unnamed Shader" };
        std::string m_VertexMSL;
        std::string m_FragmentMSL;
        MTL::Library* m_VertexLibrary{ nullptr };
        MTL::Library* m_FragmentLibrary{ nullptr };
        std::unordered_map<uint64_t, MTL::RenderPipelineState*> m_PipelineStates;
        MTL::VertexDescriptor* m_VertexDescriptor{ nullptr };
        uint64_t m_VertexDescriptorHash{ 0 };
        /* 每个顶点布局一份描述符（键 = 布局哈希）：同一个着色器配不同布局互不干扰 */
        std::unordered_map<uint64_t, MTL::VertexDescriptor*> m_VertexDescriptors;
        uint64_t m_ActiveVertexLayoutHash{ 0 };

        /* 材质参数块：顶点阶段与片元阶段各可能有一块 */
        std::vector<MaterialBlock> m_MaterialBlocks;

        /* 参数句柄 → 落在哪几块的哪个偏移（顶点与片元各一份）；名字 → 句柄，没有的记 -1 */
        struct ParamTarget
        {
            uint32_t Block[2]{};
            uint32_t Offset[2]{};
            uint32_t Count{ 0 };
        };
        std::vector<ParamTarget> m_ParamTargets;
        std::unordered_map<std::string, int> m_ParamHandles;

        /* 纹理槽位 → 使用阶段掩码（首次查询时算好） */
        std::unordered_map<uint32_t, uint8_t> m_TextureStageMasks;

        /* GLSL 裸 uniform 的默认初始值（名字 → 按 MSL 布局序列化的字节）。
         * MSL 的 constant 结构体成员不支持初始化器，跨编译后源码默认值会丢失，
         * 创建材质块时用该表还原，避免未写入的 uniform 退化为全零。 */
        std::unordered_map<std::string, std::vector<uint8_t>> m_DefaultUniformValues;

        /* 已告警过“未找到同名 uniform”的名字集合：材质每帧都会重试写入，
         * 按名字去重保证同一问题只告警一次，避免刷屏。 */
        std::unordered_set<std::string> m_WarnedMissingUniforms;

        /* 编译产物可用（有 MSL）；内嵌源码实例恒为 true */
        bool m_Loaded{ false };

        /* 建失败过的管线参数哈希（随重载一起换掉） */
        std::unordered_set<uint64_t> m_FailedPipelines;

        /* 把另一份实例的编译产物、反射与材质块换到本对象，旧状态留在 other 由它析构释放。
         * 顶点描述符不换：它来自外部顶点数组，只在属性位置变了时才作废 */
        void StealFrom(MetalShader& other);

        /* 重载时要搬过去的材质块数值：名字之外带上类型，类型或大小对不上就不搬 */
        struct SavedMaterialValue
        {
            std::string Type{};
            std::vector<uint8_t> Bytes{};
        };

        /* 材质块当前值：成员名 → 类型与字节 */
        [[nodiscard]] std::unordered_map<std::string, SavedMaterialValue> SnapshotMaterialValues() const;
        /* 按名字写回新布局：同名同类型才搬，新加或改过类型的成员保留源码默认值 */
        void RestoreMaterialValues(const std::unordered_map<std::string, SavedMaterialValue>& values);
    };
}

#endif /* HELIOS_PLATFORM_APPLE */
