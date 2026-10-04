#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace Helios
{
    class ShaderCompiler
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

        /* 读取 shader 文件，递归处理 #include "file" 和 #include <file> */
        static std::string ReadFile(const std::string& filepath);

        /* 按 #type vertex / #type fragment / #type compute 切分源码 */
        static void PreProcessShaderSrc(const std::string& source,
            std::unordered_map<ShaderStage, std::string>& shader_sources);

        /* 把 GLSL 源码编译到目标平台的着色器（name 只用于缓存文件名和注释）。先按 (source hash, stage,
         * target) 查缓存，命中直接返回；否则走 SPIR-V 翻译并写缓存。失败返回空字符串。 */
        static std::string Compile(const std::string& source, ShaderStage stage, ShaderTarget target,
            const std::string& name = "");

        /* 带错误信息的编译接口，返回 true 表示成功。 */
        static bool Compile(const std::string& source, ShaderStage stage, ShaderTarget target,
            std::string& output, std::string& error_msg, const std::string& name = "");

    private:
        static std::string TargetToString(ShaderTarget target);
        static std::string GetFileExtension(ShaderTarget target);
        static std::string ComputeHash(const std::string& source);
        static std::filesystem::path GetCacheDirectory(ShaderTarget target);

        static bool TryLoadCache(const std::string& cache_path, std::string& output);
        static void SaveCache(const std::string& cache_path, const std::string& output);

        static void PrependSourceInfo(std::string& output, const std::string& name, ShaderStage stage, ShaderTarget target);

        static bool CompileInternal(const std::string& source, ShaderStage stage, ShaderTarget target,
            std::string& output, std::string& error_msg, const std::string& name);
        static bool CompileGLSLToSPIRV(const std::string& glsl_source, ShaderStage stage,
            std::vector<uint32_t>& spirv, std::string& error_msg);
        static bool SPIRVToTarget(const std::vector<uint32_t>& spirv, const std::string& glsl_source,
            ShaderStage stage, ShaderTarget target, std::string& output, std::string& error_msg);
    };
}
