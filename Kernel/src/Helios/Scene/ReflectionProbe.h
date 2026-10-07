#pragma once
#include "SceneObject.h"
#include "ReflectionProbeBakeCache.h"

namespace Helios
{
    class DeviceTexture;
    class DeviceFrameBuffer;
    class RenderView;

    /* 反射探针：在指定位置烘焙IBL反射 */
    class ReflectionProbe : public SceneObject
    {
    public:
        /* IBL烘焙参数 */
        struct BakeConfig
        {
            uint32_t EnvSize = 512;
            uint32_t IrradianceSize = 32;
            uint32_t PrefilterSize = 128;
            uint32_t PrefilterMipLevels = 0;/* 0-表示根据PrefilterSize自动推导 */
            bool BakeSkyBoxOnly = true;     /* 仅烘焙天空盒 */
            bool BakeDiffuse = true;        /* 是否需要烘焙漫反射(irradiance) */
            bool BakeSpecular = true;       /* 是否需要烘焙镜面反射(prefilter) */
        };

        ReflectionProbe();
        ~ReflectionProbe() = default;

        /* 设置用于烘焙的等距柱状(equirectangular)源纹理（通常是天空盒贴图） */
        void SetSkyBoxTexture(SharedPtr<DeviceTexture> skybox_tex) { m_pSkyBoxTexture = skybox_tex; }
        SharedPtr<DeviceTexture> GetSkyBoxTexture() const { return m_pSkyBoxTexture; }

        /* 设置烘焙配置参数 */
        void SetBakeConfig(const BakeConfig& config) { m_BakeConfig = config; }
        const BakeConfig& GetBakeConfig() const { return m_BakeConfig; }

        /* 设置每帧烘焙 */
        void SetRealtime(bool realtime) { m_IsRealtime = realtime; }
        bool IsRealtime() const { return m_IsRealtime; }

        /* 执行烘焙 */
        void Bake(RenderView* render_view);
        /* 重置烘焙状态、下次渲染重新烘焙。注意：它会当场销毁烘焙结果和预览纹理 —— 这些可能仍被
         * 当前帧的 ImGui 绘制列表引用，直接销毁会留下悬垂指针。UI（Rebake 按钮）走 RequestRebake 推迟执行。 */
        void Reset();

        /* 请求重烘焙：只在 UI 帧里置位，真正的 Reset 推迟到下一帧的 TickBakePreview
         * （Prepare 阶段、先于绘制/UI）执行 —— 那时旧纹理已不在任何绘制列表里。 */
        void RequestRebake();

        /* 获取烘焙结果 */
        SharedPtr<DeviceTexture> GetEnvCubemap() const { return m_BakeResult.EnvColorCubemap; }
        SharedPtr<DeviceTexture> GetIrradianceMap() const { return m_BakeResult.IrradianceMap; }
        SharedPtr<DeviceTexture> GetPrefilterMap() const { return m_BakeResult.PrefilterMap; }

        /* Probe已经烘焙 */
        bool IsBaked() const { return m_BakeCompleted; }

        /* ---- 烘焙结果的磁盘缓存 ----
         * 一组 HDR 立方体贴图可以落盘复用（加载场景直接恢复、跳过烘焙）。写入时机 = 保存场景
         * （Scene::Serializer 登记请求）；编辑过程中的高频中间状态不持久化。 */

        /* 缓存写入的状态机：等待烘焙完成 -> 等待 GPU 执行完 -> 回读落盘 */
        enum class BakeCacheWriteState : uint8_t
        {
            Idle = 0,
            Requested,
            WaitingForGPU,
        };

        /* 缓存文件路径（相对资源根）；为空表示不持久化 */
        void SetBakeCachePath(const std::string& path) { m_BakeCachePath = path; }
        [[nodiscard]] const std::string& GetBakeCachePath() const { return m_BakeCachePath; }

        /* 请求把当前烘焙结果写入 path（相对资源根，同时记为持久化路径）。
         * 由 Scene::Serializer 在保存场景时登记，实际写盘由 TickBakeCacheWrite 推进。 */
        void RequestBakeCacheWrite(const std::string& path);

        /* 默认缓存位置：BakedReflectionProbes/<场景相对路径去扩展名>/<探针名>.probe。缓存按场景分
         * 目录（镜像场景的目录结构，避免互相覆盖）；场景不在资源根下时就按场景文件名建目录，
         * scene_path 为空则退回旧的平铺布局。 */
        [[nodiscard]] std::string MakeDefaultBakeCachePath(const std::string& scene_path) const;

        /* 每帧推进写入状态机（由 ReflectionProbeManager 驱动）。
         * 落盘必须等 GPU 执行完承载烘焙绘制的命令缓冲区，否则 Shared 存储模式下读到的是旧内容。 */
        void TickBakeCacheWrite();

        /* 立即写出 / 读入缓存（路径为绝对路径）；写入要求 GPU 已完成本次烘焙的绘制 */
        bool SaveBakeCache(const std::string& path);
        bool LoadBakeCache(const std::string& path);

        [[nodiscard]] BakeCacheWriteState GetBakeCacheWriteState() const { return m_CacheWriteState; }
        [[nodiscard]] bool HasBakeCacheWriteFailed() const { return m_CacheWriteFailed; }

        /* ---- 烘焙结果的预览图 ----
         * 立方体贴图 UI 没法直接采样：按需转成 RGBA8 的十字展开图（4x3 布局）给属性面板显示。
         * 懒生成（第一次显示或者 Rebake 重建时才做）。 */

        enum class BakePreviewKind : uint8_t
        {
            Environment = 0,
            Irradiance,
            Prefilter,
            Count,
        };

        enum class BakePreviewState : uint8_t
        {
            Idle = 0,
            Requested,      /* 已请求：等烘焙完成 */
            WaitingForGPU,  /* 烘焙完成：等 GPU 执行完再回读 */
        };

        /* 请求生成（或刷新）预览。幂等：生成中重复调用不生效；
         * 失败后不再自动重试（重烘焙 Reset 会重新武装）。 */
        void RequestBakePreview();

        /* 每帧推进预览生成状态机（由 ReflectionProbeManager 驱动，与缓存写入同一帧序） */
        void TickBakePreview();

        /* 预览图（懒生成，未生成时为空）。Environment 一定有（烘焙总产出环境图），
         * Irradiance / Prefilter 取决于烘焙配置。 */
        [[nodiscard]] SharedPtr<DeviceTexture> GetBakePreviewTexture(BakePreviewKind kind) const
        {
            return m_BakePreviews[static_cast<size_t>(kind)];
        }
        [[nodiscard]] BakePreviewState GetBakePreviewState() const { return m_PreviewState; }
        [[nodiscard]] bool HasBakePreview() const { return m_BakePreviews[0] != nullptr; }
        [[nodiscard]] bool HasBakePreviewFailed() const { return m_PreviewFailed; }

        /* 把烘焙数据（立方图，含 mip）转成十字展开的 RGBA8 预览纹理：half / float 解码、Reinhard +
         * gamma、拼 4x3（空角透明）、上传 2D 纹理。mip_index < 0 = 自动（≤64px 那层）、≥ 0 = 指定层级。
         * 返回空 = 上下文缺失 / 格式不支持 / 数据不完整 / 越界。 */
        static SharedPtr<DeviceTexture> BuildPreviewTexture(const std::string& name,
            const ReflectionProbeBakeCache::Image& image, int mip_index = -1);

        /* 自动选取的预览 mip 序号（封面到 ≤64px 的那层）—— 与 BuildPreviewTexture 的
         * 默认行为一致；资源详情用它把"Mip"组合框的初始选中项设成同一层。 */
        static uint32_t AutoPreviewMip(const ReflectionProbeBakeCache::Image& image);

        /* 某层 mip 的立方图面边长：max(1, Size >> mip)（与缓存文件写读的校验口径一致）。
         * 资源详情的 Mip 组合框用它给每层拼标签。 */
        static uint32_t PreviewMipFaceSize(uint32_t mip0_size, uint32_t mip_index);

    private:
        SharedPtr<DeviceFrameBuffer> MakeSceneCaptureFrameBuffer(const SharedPtr<DeviceTexture>& color_target, const SharedPtr<DeviceTexture>& depth_target, uint32_t size);
        /* 生成环境立方体贴图：BakeSkyBoxOnly 时用 EquirectToCube 生成；否则在探针位置用 6 个朝向的
         * 相机实时捕获场景几何。返回的立方图供后续烘焙 IrradianceMap 和 PrefilterMap。 */
        void BakeEnvCubemap(RenderView* render_view);
        /* 烘焙IrradianceMap */
        void BakeIrradianceMap();
        /* 烘焙PrefilterMap */
        void BakePrefilterMap();

        /* 生成预览图：把三张烘焙立方图回读、色调映射、拼成十字展开、上传为 2D 纹理。
         * 需要 GPU 已完成烘焙绘制（调用方先 WaitForGPU）。 */
        bool BuildBakePreviews();

        /* IBL烘焙结果 */
        struct BakeResult
        {
            SharedPtr<DeviceTexture> EnvColorCubemap{ nullptr };    /* 环境立方体贴图：颜色图 */
            SharedPtr<DeviceTexture> EnvDepthCubemap{ nullptr };    /* 环境立方体贴图：深度图（渲染场景时需要） */
            SharedPtr<DeviceTexture> IrradianceMap{ nullptr };      /* 漫反射辐照度立方体贴图 */
            SharedPtr<DeviceTexture> PrefilterMap{ nullptr };       /* 预滤波(粗糙度)立方体贴图(mip 分级) */
        };

        SharedPtr<DeviceTexture> m_pSkyBoxTexture;
        BakeConfig m_BakeConfig{};
        BakeResult m_BakeResult{};
        bool m_IsRealtime{ false };
        bool m_BakeCompleted{ false };

        /* 烘焙结果缓存 */
        std::string m_BakeCachePath{};
        BakeCacheWriteState m_CacheWriteState{ BakeCacheWriteState::Idle };
        /* 烘焙完成后还需等待的帧数，让承载烘焙绘制的命令缓冲区先提交 */
        uint8_t m_CacheWriteCountdown{ 0 };
        bool m_CacheWriteFailed{ false };

        /* 烘焙结果预览（十字展开图）：懒生成状态机，由 UI 请求触发 */
        static constexpr size_t kBakePreviewCount = static_cast<size_t>(BakePreviewKind::Count);
        SharedPtr<DeviceTexture> m_BakePreviews[kBakePreviewCount]{};
        BakePreviewState m_PreviewState{ BakePreviewState::Idle };
        uint8_t m_PreviewCountdown{ 0 };
        bool m_PreviewFailed{ false };
        /* 重烘焙请求（UI 帧里只置位，TickBakePreview 里执行，见 RequestRebake） */
        bool m_RebakeRequested{ false };

        friend class ReflectionProbeManager;
    };

    /* 统一管理场景中所有ReflectionProbe */
    class ReflectionProbeManager
    {
    public:
        ReflectionProbeManager() = default;
        ~ReflectionProbeManager() = default;
        ReflectionProbeManager(const ReflectionProbeManager&) = delete;
        ReflectionProbeManager& operator=(const ReflectionProbeManager&) = delete;

        /* 注册一个需要烘焙的反射探针（由 ReflectionProbeComponent 创建时调用） */
        void RegisterProbe(const SharedPtr<ReflectionProbe>& probe);
        /* 注销一个反射探针（由 ReflectionProbeComponent 销毁时调用） */
        void UnregisterProbe(const SharedPtr<ReflectionProbe>& probe);

        /* 清空所有已注册的探针（场景内容被整体替换时调用） */
        void ClearProbes();

        /* 是否存在已注册的反射探针 */
        bool HasProbe() const { return !m_RegisteredProbes.empty(); }

        /* 获取距离最近的反射探针 */
        SharedPtr<ReflectionProbe> GetClostedReflectionProbe(const glm::vec3& target_pos) const;

        /* 按到目标点的距离升序收集可用的已烘焙探针（最多 max_count 个；
         * "可用"= 启用 + 已烘焙 + 双图齐备）。延迟光照据此决定本帧参与
         * 逐像素选择的探针子集。 */
        std::vector<SharedPtr<ReflectionProbe>> CollectClosestBakedProbes(const glm::vec3& target_pos,
            size_t max_count) const;

        /* 获取BRDF查找表贴图 */
        const SharedPtr<DeviceTexture>& GetBRDFLutMap() const { return m_BRDFLutMap; }

        /* 从已注册的反射探针中，筛选当前帧需要烘焙（realtime 或 dirty）的探针。 */
        void Prepare();

        /* 为所有已烘焙的探针登记缓存写入请求（由 Scene::Serializer 在保存场景时调用）。
         * scene_path 为正在保存的场景文件路径：缓存路径在保存时按场景重新推导，
         * 场景文件里记录的旧路径只用于加载；没有烘焙结果的探针会被跳过。 */
        void RequestBakeCacheWrites(const std::string& scene_path);

        /* 将 BRDFLut 烘焙 Pass 与所有需要烘焙的 ReflectionProbe 的 Bake Pass 注入到当前 RenderView 的 FrameGraph 中。
         * BRDFLut 纹理句柄会被写入 FrameGraph 的 Blackboard（"ReflectionProbeBRDFLutHandle"），供后续 Pass 使用。 */
        void AddBakeReflectionProbePass(RenderView* render_view);

    private:
        /* 烘焙BRDF查找表 */
        void BakeBRDFLutMap();

        /* 已注册的反射探针（由 ReflectionProbeComponent 注册，随组件销毁注销） */
        std::vector<SharedPtr<ReflectionProbe>> m_RegisteredProbes{};
        /* 当前帧需要烘焙的探针（由 Prepare 收集） */
        std::vector<SharedPtr<ReflectionProbe>> m_NeedBakeProbes{};
        /* BRDF查找表，全局仅需一张 */
        SharedPtr<DeviceTexture> m_BRDFLutMap{ nullptr };
    };
}
