#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>
#include "Helios/Common/Export.h"
#include "Helios/Renderer/RenderCommon.h"

namespace Helios
{
    /* 一次加载的附加信息：热重载据此判断"改没改"以及"改的是不是它的依赖" */
    struct ShaderSourceInfo
    {
        /* 展开 include、注入变体、分配槽位之后的各阶段源码哈希（含翻译器版本） */
        std::string Hash{};
        /* 原始文件字节的指纹（路径 + 内容 + 翻译器版本），跟展开用的是同一次读取，
         * 用来挡住"只碰了文件"的重载，比展开与分配槽位便宜得多 */
        std::string RawHash{};
        /* 递归展开到的全部文件（主文件排在最前，去重后的绝对路径） */
        std::vector<std::string> Dependencies{};
    };

    /* 编辑器也要用它读依赖（属性面板列 include），所以跨模块导出 */
    class HELIOS_API ShaderCompiler
    {
    public:
        enum class ShaderStage
        {
            Vertex,
            Fragment,
            Compute
        };

        enum class ShaderTarget
        {
            Metal,
            OpenGL,
            Vulkan
        };

        static ShaderStage StringToShaderStage(const std::string& type);
        static const char* ShaderStageToString(ShaderStage stage);

        /* 读取 shader 文件，递归处理 #include "file" 和 #include <file>。
         * dependencies 非空时收集展开到的文件（绝对路径，去重），供热重载判定失效范围 */
        static std::string ReadFile(const std::string& filepath,
            std::vector<std::string>* dependencies = nullptr);

        /* 只读文件算内容指纹，口径与加载时记下的 RawHash 一致；读不到返回空串。
         * 热重载先用它挡掉"文件没变"，再谈编译。 */
        static std::string HashDependencies(const std::vector<std::string>& dependencies);

        /* 按 #type vertex / #type fragment / #type compute 切分源码 */
        static void PreProcessShaderSrc(const std::string& source,
            std::unordered_map<ShaderStage, std::string>& shader_sources);

        /* 自动分配槽位，源码里不用手写（写了也会被替换）：裸 uniform 的 location、sampler 的 binding、
         * 顶点 out 与片元 in 的 location（按名字配对）。各阶段按首次出现的顺序编号，同名同号；
         * write_uniform_locations 为 false 时裸 uniform 只去掉不写。出错返回 false，源码保持原样。 */
        static bool AssignShaderSlots(std::unordered_map<ShaderStage, std::string>& stage_sources,
            bool write_uniform_locations, std::string& error_msg);

        /* 后端统一从这里拿各阶段源码：读文件、展开 include、按 #type 拆阶段，再插变体 define、
         * 自动分配槽位。defines 为空时与不带变体的行为完全一致。
         * info 非空时输出内容哈希与依赖表（加载完成后不再重读文件）。
         * 读取或分配失败只打日志；读不到文件时返回空表。 */
        static std::unordered_map<ShaderStage, std::string> LoadStageSources(const std::string& filepath,
            ShaderTarget target, const ShaderVariantDefines& defines = {}, ShaderSourceInfo* info = nullptr);
        /* 内嵌源码只做槽位分配；裸 uniform 的 location 只有 Windows 的 GL SPIR-V 路径要写 */
        static void PrepareStageSources(std::unordered_map<ShaderStage, std::string>& stage_sources,
            ShaderTarget target, const std::string& name);

        /* 把 GLSL 源码编译到目标平台的着色器（name 只用于缓存文件名和注释）。先按 (source hash, stage,
         * target) 查缓存，命中直接返回；否则走 SPIR-V 翻译并写缓存。失败返回空字符串。 */
        static std::string Compile(const std::string& source, ShaderStage stage, ShaderTarget target,
            const std::string& name = "");

        /* 带错误信息的编译接口，返回 true 表示成功。 */
        static bool Compile(const std::string& source, ShaderStage stage, ShaderTarget target,
            std::string& output, std::string& error_msg, const std::string& name = "");

        /* 源码内容哈希（含翻译器版本），编译缓存按它命名 */
        static std::string ComputeHash(const std::string& source);

    private:
        /* 一次读取的上下文：依赖表、各文件原始字节、当前 include 链（查循环用） */
        struct IncludeContext
        {
            std::vector<std::string>* Dependencies{ nullptr };
            std::vector<std::pair<std::string, std::string>>* Contents{ nullptr };
            std::vector<std::string> Stack{};
        };

        static std::string ReadFile(const std::string& filepath, IncludeContext& context);
        /* 依赖文件的原始字节指纹；有文件读不到时返回空串 */
        static std::string HashContents(const std::vector<std::pair<std::string, std::string>>& contents);

        static std::string TargetToString(ShaderTarget target);
        static std::string GetFileExtension(ShaderTarget target);
        static std::filesystem::path GetCacheDirectory(ShaderTarget target);

        static bool TryLoadCache(const std::string& cache_path, std::string& output);
        static void SaveCache(const std::string& cache_path, const std::string& output);

        static void PrependSourceInfo(std::string& output, const std::string& name, ShaderStage stage, ShaderTarget target);

        /* 把变体维度拼成 #define 前缀插进每个阶段：#version 之后（GLSL 要求它排第一）、声明之前。
         * #type 切分后每阶段各插一份 —— 插到文件头只有第一阶段拿得到。 */
        static void InjectVariantDefines(std::unordered_map<ShaderStage, std::string>& stage_sources,
            const ShaderVariantDefines& defines);

        static bool CompileInternal(const std::string& source, ShaderStage stage, ShaderTarget target,
            std::string& output, std::string& error_msg, const std::string& name);
        static bool CompileGLSLToSPIRV(const std::string& glsl_source, ShaderStage stage,
            std::vector<uint32_t>& spirv, std::string& processed_source, std::string& error_msg);

        /* ---- 深度纹理采样标注 ----
         * 源码里写 `// @depth-texture <采样器名>`：该采样器在 SPIR-V 里标成深度图，spirv-cross 会生成
         * MSL 的 depth2d<float>。深度 / 深度模板格式不能按 texture2d 绑定（Metal 校验按声明断言）。 */
        static std::vector<std::string> CollectDepthTextureAnnotations(const std::string& source);
        /* 名 -> 预处理后源码中的 binding -> 修改 SPIR-V 中对应图像的 Depth 标志 */
        static void ApplyDepthTextureAnnotations(const std::string& processed_source,
            const std::vector<std::string>& annotations, std::vector<uint32_t>& spirv);
        /* 把指定 binding 的组合采样器图像标记为深度图（OpTypeImage.Depth = 1）。
         * 组合采样器在 SPIR-V 中拆成 image + sampler 两个变量（同 binding），
         * 变量经 OpTypePointer 指向 OpTypeSampledImage，再指向 OpTypeImage。 */
        static bool MarkImageAsDepth(std::vector<uint32_t>& spirv, uint32_t binding);
        static bool SPIRVToTarget(const std::vector<uint32_t>& spirv, const std::string& glsl_source,
            ShaderStage stage, ShaderTarget target, std::string& output, std::string& error_msg);
    };
}
