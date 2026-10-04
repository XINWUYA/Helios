#pragma once
#include "SceneObject.h"

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
        /* 重置烘焙状态，使下一次渲染重新烘焙 */
        void Reset();

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

        /* 默认缓存位置：BakedReflectionProbes/<探针名>.probe（相对资源根） */
        [[nodiscard]] std::string MakeDefaultBakeCachePath() const;

        /* 每帧推进写入状态机（由 ReflectionProbeManager 驱动）。
         * 落盘必须等 GPU 执行完承载烘焙绘制的命令缓冲区，否则 Shared 存储模式下读到的是旧内容。 */
        void TickBakeCacheWrite();

        /* 立即写出 / 读入缓存（路径为绝对路径）；写入要求 GPU 已完成本次烘焙的绘制 */
        bool SaveBakeCache(const std::string& path);
        bool LoadBakeCache(const std::string& path);

        [[nodiscard]] BakeCacheWriteState GetBakeCacheWriteState() const { return m_CacheWriteState; }
        [[nodiscard]] bool HasBakeCacheWriteFailed() const { return m_CacheWriteFailed; }

    private:
        SharedPtr<DeviceFrameBuffer> MakeSceneCaptureFrameBuffer(const SharedPtr<DeviceTexture>& color_target, const SharedPtr<DeviceTexture>& depth_target, uint32_t size);
        /* 生成环境立方体贴图：BakeSkyBoxOnly 时用 EquirectToCube 生成；否则在探针位置用 6 个朝向的
         * 相机实时捕获场景几何。返回的立方图供后续烘焙 IrradianceMap 和 PrefilterMap。 */
        void BakeEnvCubemap(RenderView* render_view);
        /* 烘焙IrradianceMap */
        void BakeIrradianceMap();
        /* 烘焙PrefilterMap */
        void BakePrefilterMap();

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

        /* 是否存在已注册的反射探针 */
        bool HasProbe() const { return !m_RegisteredProbes.empty(); }

        /* 获取距离最近的反射探针 */
        SharedPtr<ReflectionProbe> GetClostedReflectionProbe(const glm::vec3& target_pos) const;

        /* 获取BRDF查找表贴图 */
        const SharedPtr<DeviceTexture>& GetBRDFLutMap() const { return m_BRDFLutMap; }

        /* 从已注册的反射探针中，筛选当前帧需要烘焙（realtime 或 dirty）的探针。 */
        void Prepare();

        /* 为所有已烘焙的探针登记缓存写入请求（由 Scene::Serializer 在保存场景时调用）。
         * 没有烘焙结果的探针会被跳过：此时没有需要持久化的内容。 */
        void RequestBakeCacheWrites();

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
