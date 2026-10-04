#pragma once

#ifdef PLATFORM_MACOS

#include "Helios/VirtualDevice/DeviceShader.h"
#include "Helios/Renderer/RenderCommon.h"
#include <Metal/Metal.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Helios
{
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
        MetalShader(const std::string& filepath);
        MetalShader(std::string name, const std::string& vertex_src, const std::string& pixel_src);
        ~MetalShader() override;

        void Bind() override;
        void Unbind() override;

        void BindVertexArray(const SharedPtr<DeviceVertexArray>& vertex_array) override;

        int GetUniformLocation(const std::string& name) override;
        void SetInt(const std::string& name, int value) override;
        void SetIntArray(const std::string& name, int* values, uint32_t count) override;
        void SetFloat(const std::string& name, float value) override;
        void SetFloat2(const std::string& name, const glm::vec2& value) override;
        void SetFloat3(const std::string& name, const glm::vec3& value) override;
        void SetFloat4(const std::string& name, const glm::vec4& value) override;
        void SetMat4(const std::string& name, const glm::mat4& value) override;

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

    private:
        /* 着色器内嵌的一块材质参数（由 SPIRV-Cross 生成为单个 constant buffer） */
        struct MaterialBlock
        {
            uint32_t BufferIndex{ 0 };                        /* MSL buffer 索引 */
            uint32_t Size{ 0 };                               /* 数据大小（字节） */
            bool IsVertexStage{ false };                      /* 绑定的着色阶段 */
            bool UseInlineBytes{ true };                      /* 是否走 setBytes 快速路径 */
            MTL::Buffer* Buffer{ nullptr };                   /* 大块数据的后备缓冲区 */
            std::unordered_map<std::string, uint32_t> Offsets; /* 成员名 → 字节偏移 */
            std::vector<uint8_t> Shadow;                      /* CPU 端影子数据 */
        };

        /* 编译并反射着色器：生成 MSL、提取材质参数布局与资源槽位 */
        void CompileAndReflect(const std::string& vertex_src, const std::string& fragment_src);

        /* 从 MSL 中解析材质参数块与纹理/采样器槽位 */
        void ParseShaderResources(const std::string& msl_source, bool is_vertex_stage);
        void ExtractMaterialBlocks(const std::string& msl_source, bool is_vertex_stage);
        void ExtractTextureAndSamplerBindings(const std::string& msl_source);

        bool EnsureShaderLibraries();
        MTL::RenderPipelineState* GetOrCreatePipelineState(const MetalPipelineDesc& desc);
        void ReleaseMaterialBlocks();

        /* 在全部材质块中查找成员，命中时输出所在块与偏移 */
        MaterialBlock* FindMaterialMember(const std::string& name, uint32_t& out_offset);
        void WriteMaterialData(const std::string& name, const void* data, uint32_t size);

        std::string m_DebugName{ "Unnamed Shader" };
        std::string m_VertexMSL;
        std::string m_FragmentMSL;
        MTL::Library* m_VertexLibrary{ nullptr };
        MTL::Library* m_FragmentLibrary{ nullptr };
        std::unordered_map<uint64_t, MTL::RenderPipelineState*> m_PipelineStates;
        MTL::VertexDescriptor* m_VertexDescriptor{ nullptr };
        uint64_t m_VertexDescriptorHash{ 0 };

        /* 材质参数块：顶点阶段与片元阶段各可能有一块 */
        std::vector<MaterialBlock> m_MaterialBlocks;

        /* GLSL 裸 uniform 的默认初始值（名字 → 按 MSL 布局序列化的字节）。
         * MSL 的 constant 结构体成员不支持初始化器，跨编译后源码默认值会丢失，
         * 创建材质块时用该表还原，避免未写入的 uniform 退化为全零。 */
        std::unordered_map<std::string, std::vector<uint8_t>> m_DefaultUniformValues;

        /* 已告警过“未找到同名 uniform”的名字集合：材质每帧都会重试写入，
         * 按名字去重保证同一问题只告警一次，避免刷屏。 */
        std::unordered_set<std::string> m_WarnedMissingUniforms;
    };
}

#endif /* PLATFORM_MACOS */
