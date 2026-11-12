#include "Pch.h"
#include "ReflectionProbe.h"
#include "SceneCommon.h"
#include "Helios/Renderer/FrameGraph/FrameGraph.h"
#include <Helios/Renderer/RenderView.h>
#include <Helios/Renderer/Renderer.h>
#include <Helios/Renderer/RenderCommon.h>
#include <Helios/Common/Math.h>
#include <Helios/Core/AssetManager.h>
#include <Helios/Scene/Camera.h>
#include <Helios/Scene/Mesh.h>
#include <Helios/Scene/Material.h>
#include <Helios/VirtualDevice/DeviceTexture.h>
#include <Helios/VirtualDevice/DeviceShader.h>
#include <Helios/VirtualDevice/DeviceFrameBuffer.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <unordered_set>
#include <vector>

namespace Helios
{
    void ReflectionProbeManager::RegisterProbe(const SharedPtr<ReflectionProbe>& probe)
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

        if (!probe)
            return;
        if (std::find(m_RegisteredProbes.begin(), m_RegisteredProbes.end(), probe) == m_RegisteredProbes.end())
            m_RegisteredProbes.push_back(probe);
    }

    void ReflectionProbeManager::UnregisterProbe(const SharedPtr<ReflectionProbe>& probe)
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

        auto it = std::find(m_RegisteredProbes.begin(), m_RegisteredProbes.end(), probe);
        if (it != m_RegisteredProbes.end())
            m_RegisteredProbes.erase(it);
    }

    void ReflectionProbeManager::ClearProbes()
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

        m_RegisteredProbes.clear();
        m_NeedBakeProbes.clear();
    }

    namespace
    {
        /* 探针当前可否用于 IBL 着色：启用、已烘焙、双图（辐照度 + 预滤波）齐备，
         * 且探针所在层与视图可见层相交。缺图（烘焙配置关掉了漫反射 / 镜面）时按"不可用"
         * 处理 —— 采样未绑定的立方图在 Metal 校验层直接断言，不做猜测性兜底。 */
        bool IsProbeUsableForIBL(const SharedPtr<ReflectionProbe>& probe, LayerMask culling_mask)
        {
            return probe != nullptr && probe->GetEnable() && probe->IsBaked()
                && Layers::IsVisible(probe->GetEntityLayerMask(), culling_mask)
                && probe->GetIrradianceMap() != nullptr && probe->GetPrefilterMap() != nullptr;
        }

        /* 盒内深度：到最近盒面的余量（出盒为负） */
        float ProbeBoxDepth(const ReflectionProbe& probe, const glm::vec3& position)
        {
            const glm::vec3 half_size = probe.GetBoxSize() * 0.5f;
            const glm::vec3 delta = glm::abs(position - probe.GetPosition());
            return glm::min(glm::min(half_size.x - delta.x, half_size.y - delta.y), half_size.z - delta.z);
        }

        /* 盒体权重（与 Lighting.glsl 的 ProbeBoxWeight 同口径）：
         * 盒内深度过 BlendDistance 拉满；BlendDistance = 0 时盒内即满权重（阶跃） */
        float ProbeBoxWeight(const ReflectionProbe& probe, const glm::vec3& position)
        {
            const float depth = ProbeBoxDepth(probe, position);
            const float blend = probe.GetBlendDistance();
            if (blend > 0.0f)
                return glm::clamp(depth / blend, 0.0f, 1.0f);
            return depth > 0.0f ? 1.0f : 0.0f;
        }
    }

    /* 获取用于 IBL 的反射探针（前向 per-draw）：启用混合时选"盒体权重最高"的（权重并列
     * 或全部出盒时比距离）；未启用（没有探针配 BlendDistance）时就是距离最近的旧行为。 */
    SharedPtr<ReflectionProbe> ReflectionProbeManager::GetClostedReflectionProbe(const glm::vec3& target_pos,
        LayerMask culling_mask) const
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

        const bool blend_enabled = IsProbeBlendingEnabled(culling_mask);

        float best_weight = -1.0f;
        float best_dist_sq = std::numeric_limits<float>::max();
        SharedPtr<ReflectionProbe> chosen = nullptr;

        for (const auto& probe : m_RegisteredProbes)
        {
            if (!IsProbeUsableForIBL(probe, culling_mask))
                continue;

            const float dist_sq = glm::distance2(target_pos, probe->GetPosition());
            if (!blend_enabled)
            {
                if (dist_sq < best_dist_sq)
                {
                    best_dist_sq = dist_sq;
                    chosen = probe;
                }
                continue;
            }

            const float weight = ProbeBoxWeight(*probe, target_pos);
            if (weight > best_weight || (weight == best_weight && dist_sq < best_dist_sq))
            {
                best_weight = weight;
                best_dist_sq = dist_sq;
                chosen = probe;
            }
        }

        return chosen;
    }

    bool ReflectionProbeManager::IsProbeBlendingEnabled(LayerMask culling_mask) const
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

        for (const auto& probe : m_RegisteredProbes)
        {
            if (IsProbeUsableForIBL(probe, culling_mask) && probe->GetBlendDistance() > 0.0f)
                return true;
        }
        return false;
    }

    /* 按到目标点的距离升序收集可用的已烘焙探针（最多 max_count 个）。
     * 延迟光照在着色器里逐像素做"最近探针"选择，这里给出本帧参与选择的子集
     * （调用方传入相机位置：离视点更近的探针优先获得采样器槽位）。 */
    std::vector<SharedPtr<ReflectionProbe>> ReflectionProbeManager::CollectClosestBakedProbes(
        const glm::vec3& target_pos, size_t max_count, LayerMask culling_mask) const
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

        std::vector<SharedPtr<ReflectionProbe>> usable;
        usable.reserve(m_RegisteredProbes.size());
        for (const auto& probe : m_RegisteredProbes)
        {
            if (IsProbeUsableForIBL(probe, culling_mask))
                usable.push_back(probe);
        }

        std::sort(usable.begin(), usable.end(),
            [&target_pos](const SharedPtr<ReflectionProbe>& lhs, const SharedPtr<ReflectionProbe>& rhs)
            {
                return glm::distance2(target_pos, lhs->GetPosition())
                    < glm::distance2(target_pos, rhs->GetPosition());
            });

        if (usable.size() > max_count)
            usable.resize(max_count);
        return usable;
    }

    void ReflectionProbeManager::Prepare()
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

        m_NeedBakeProbes.clear();

        /* 从已注册的反射探针中，筛选需要烘焙的（realtime 探针每帧烘焙，未烘焙的 dirty 探针需要烘焙） */
        for (const auto& probe : m_RegisteredProbes)
        {
            if (!probe || !probe->GetEnable())
                continue;

            /* 推进「烘焙结果落盘」状态机：等烘焙完成、再等 GPU 执行完，最后回读写出 */
            probe->TickBakeCacheWrite();

            /* 推进「烘焙结果预览」状态机：同样等烘焙完成 + GPU 执行完，再转换上传 */
            probe->TickBakePreview();

            if (probe->NeedsRealtimeBake() || !probe->IsBaked())
            {
                /* 烘焙间隔：没到点的这一帧不挂 Pass，探针完全不进渲染图 */
                if (!probe->TickBakeInterval())
                    continue;

                m_NeedBakeProbes.push_back(probe);
            }
        }
    }

    void ReflectionProbeManager::RequestBakeCacheWrites(const std::string& scene_path)
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

        /* 同一场景允许存在重名探针（复制实体不会自动改名）：按登记顺序给重名者的
         * 缓存文件加 _2 / _3 … 序号，否则它们会互相覆盖彼此的烘焙结果。
         * 登记顺序与实体创建顺序一致，加载后仍然稳定。 */
        std::unordered_set<std::string> assigned_paths;

        for (const auto& probe : m_RegisteredProbes)
        {
            /* 没有烘焙结果就没有需要持久化的内容 */
            if (!probe || !probe->GetEnable() || !probe->IsBaked())
                continue;

            /* 写入目标在保存时重新推导（缓存目录跟随场景）：场景另存/搬移后缓存跟着走，
             * 场景文件里记录的旧路径只用于加载。 */
            std::string path = probe->MakeDefaultBakeCachePath(scene_path);

            if (!assigned_paths.insert(path).second)
            {
                constexpr char kExtension[] = ".probe";
                const std::string base = path.substr(0, path.size() - (sizeof(kExtension) - 1));
                for (uint32_t index = 2; ; ++index)
                {
                    std::string candidate = base + "_" + std::to_string(index) + kExtension;
                    if (assigned_paths.insert(candidate).second)
                    {
                        path = std::move(candidate);
                        break;
                    }
                }
            }

            probe->RequestBakeCacheWrite(path);
        }
    }

    void ReflectionProbeManager::AddBakeReflectionProbePass(RenderView* render_view)
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

        if (!render_view)
            return;

        auto& frame_graph = render_view->GetFrameGraph();
        if (!frame_graph)
            return;

        /* BRDFLut 是 IBL 的通用查找表：只要有已注册的探针就要保证它在 ——
         * 探针可能全部来自磁盘缓存（这一帧不需要任何烘焙 Pass），但 IBL 着色仍要
         * 用它。只需烘焙一次。 */
        if (m_BRDFLutMap == nullptr && !m_RegisteredProbes.empty())
            BakeBRDFLutMap();

        if (m_NeedBakeProbes.empty())
            return;

        /* 捕获走自己的视图：管线、视图变体、调试档位都不沾宿主相机 */
        if (m_pCaptureView == nullptr)
            m_pCaptureView = CreateSharedPtr<RenderView>("ReflectionProbeCaptureView");

        /* Probe 捕获使用中性阴影图，不复用主相机的级联阴影。 */
        const auto no_shadow_map_handle = frame_graph->GetBlackboard()
            .GetResourceHandle<FrameGraphTexture>("NoShadowMapHandle");

        struct BakeProbePassData
        {
            FrameGraphResourceHandleTyped<FrameGraphTexture> NoShadowMap;
        };
        for (const auto& probe : m_NeedBakeProbes)
        {
            if (!probe)
                continue;

            frame_graph->AddPass<BakeProbePassData>("BakeReflectionProbePass",
                [no_shadow_map_handle](FrameGraphBuilder& builder, BakeProbePassData& data)
                {
                    data.NoShadowMap = no_shadow_map_handle;
                    builder.BindInputResource(data.NoShadowMap, FrameGraphTexture::Usage::Sampleable);
                    builder.AsSideEffect(true);
                },
                [this, probe, render_view](const FrameGraphResources& resources, const BakeProbePassData& data)
                {
                    ScopedShadowMapBinding shadow_map(resources.Get(data.NoShadowMap).Texture);

                    /* 捕获期间全局视图变体表切到捕获视图，结束后恢复宿主视图 ——
                     * 同一张渲染图里后面的 Pass 还要按宿主视图的档位画 */
                    const ShaderVariantDefines host_defines = Renderer::GetViewVariantDefines();
                    const Camera* host_camera = render_view->GetCullingCamera();

                    const uint32_t size = std::max(1u, probe->GetBakeConfig().EnvSize);
                    m_pCaptureView->PrepareCapture(render_view->GetOwnerScene(), probe->GetCullingMask(),
                        ViewportRegion{ 0, 0, size, size });

                    probe->Bake(m_pCaptureView.get());

                    Renderer::SetViewVariantDefines(host_defines);
                    if (host_camera != nullptr)
                    {
                        Renderer::SetViewUniforms(host_camera->GetViewMatrix(),
                            host_camera->GetProjectionMatrix(), host_camera->GetPosition());
                    }
                });
        }
    }

    ReflectionProbeManager::~ReflectionProbeManager() = default;

    /* 烘焙BRDF查找表 */
    void ReflectionProbeManager::BakeBRDFLutMap()
    {
        PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

        constexpr uint32_t BRDF_LUT_SIZE = 512;

        TextureDesc brdf_lut_desc;
        brdf_lut_desc.SamplerType = SamplerType::Sampler2D;
        brdf_lut_desc.Width = BRDF_LUT_SIZE;
        brdf_lut_desc.Height = BRDF_LUT_SIZE;
        brdf_lut_desc.Format = TextureFormat::RG16F;
        brdf_lut_desc.Usage = TextureUsage::ColorAttachment | TextureUsage::Sampleable;
        brdf_lut_desc.MipLevels = 1;
        m_BRDFLutMap = DeviceTexture::Create("BRDFLutMap", brdf_lut_desc);

        RenderBufferInfo color_info;
        color_info.RenderTarget = m_BRDFLutMap;
        color_info.Level = 0;
        color_info.Layer = 0;

        FrameBufferDesc desc;
        desc.Samples = 1;
        desc.Usage = RenderBufferUsage::ColorDefault;
        desc.ViewportRegion = ViewportRegion{ 0, 0, BRDF_LUT_SIZE, BRDF_LUT_SIZE };
        desc.ColorRenderBuffers.push_back(color_info);

        auto fb = DeviceFrameBuffer::Create("BRDFLutMap_FrameBuffer", desc);
        fb->Bind();
        {
            auto shader = AssetManager::Instance().GetOrLoad<DeviceShader>(
                ABSOLUTE_PATH("Shaders/ReflectionProbe/BRDFLut.glsl"));
            auto material = Material::Create(shader);

            Renderer::Clear();
            Renderer::Submit(material, MeshPrimitive(Renderer::GetFullScreenVertexArray()));
        }
        fb->Unbind();

    }
}
