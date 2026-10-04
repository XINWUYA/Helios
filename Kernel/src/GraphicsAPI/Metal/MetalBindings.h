#pragma once

#include <cstdint>

/* Metal 的绑定槽位约定。不依赖任何 Metal / ObjC 类型 —— Metal 实现和平台无关的着色器编译器
 * 共用（MSL 的 [[buffer(N)]] / [[texture(N)]] / [[sampler(N)]] 索引都在编译期生成）。 */
namespace Helios::MetalBinding
{
    /* 顶点缓冲区占用 [VertexBufferBase, VertexBufferBase + MaxVertexBuffers) */
    static constexpr uint32_t VertexBufferBase = 0;
    static constexpr uint32_t MaxVertexBuffers = 16;

    /* Metal 的 buffer 参数表一共 31 个槽位（合法索引 0..30），
     * 顶点缓冲区与 uniform buffer 都必须落在该范围内，
     * 否则创建 MTL::Library 时会报 "'buffer' attribute parameter is out of bounds"。 */
    static constexpr uint32_t MaxBufferSlots = 31;

    /* uniform buffer 占用 [UniformBufferBase, MaxBufferSlots)，避免与顶点缓冲区冲突 */
    static constexpr uint32_t UniformBufferBase = MaxVertexBuffers;

    /* 没有显式 binding 的默认 uniform 块（GLSL 的非 block uniform）由 glslang relaxed 规则聚合、
     * 编译时赋一个专用 binding：缺省值会跟 ViewUniformBuffer（binding = 0）撞键（后设覆盖前设）。
     * 取 uniform 区间的最后一个槽位，binding 0..13 留给引擎自己的块。 */
    static constexpr uint32_t DefaultUniformBufferBinding = MaxBufferSlots - 1 - UniformBufferBase;
    static constexpr uint32_t DefaultUniformBufferIndex =
        DefaultUniformBufferBinding + UniformBufferBase;

    static_assert(DefaultUniformBufferIndex < MaxBufferSlots,
        "default uniform block index exceeds the Metal buffer argument table");

    /* 把 GLSL 的 binding 映射为 MSL 的 buffer 索引 */
    [[nodiscard]] constexpr uint32_t ToBufferIndex(uint32_t glsl_binding) noexcept
    {
        return glsl_binding + UniformBufferBase;
    }

    /* 引擎统一管理的 uniform buffer 占 [UniformBufferBase, DefaultUniformBufferIndex)
     * （GLSL binding 0..13），由 DeviceUniformBuffer 创建和绑定（View / Object / Material / Light /
     * UI 等）；着色器不该把它们当可写参数。 */
    static constexpr uint32_t MaxEngineUniformBuffers = DefaultUniformBufferBinding;

    [[nodiscard]] constexpr bool IsEngineManagedUniformBuffer(uint32_t msl_index) noexcept
    {
        return msl_index >= UniformBufferBase && msl_index < DefaultUniformBufferIndex;
    }

    /* 是否是无显式 binding 的默认 uniform 块。引擎把它视为“着色器内嵌常量”：
     * 成员由材质的 SetXxx 接口按绘制提交，而非通过 DeviceUniformBuffer。 */
    [[nodiscard]] constexpr bool IsDefaultUniformBufferIndex(uint32_t msl_index) noexcept
    {
        return msl_index == DefaultUniformBufferIndex;
    }

    /* setVertexBytes/setFragmentBytes 的单次数据上限（Metal 规范为 4KB），
     * 超过该阈值必须回退到 MTL::Buffer，否则校验层会报错。 */
    static constexpr uint32_t MaxInlineBytes = 4 * 1024;
}
