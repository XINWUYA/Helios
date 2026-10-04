#include "Pch.h"
#include "ReflectionProbe.h"
#include "SceneCommon.h"
#include "Helios/Renderer/FrameGraph/FrameGraph.h"
#include <Helios/Renderer/RenderView.h>
#include <Helios/Renderer/Renderer.h>
#include <Helios/Renderer/RenderCommon.h>
#include <Helios/Common/Math.h>
#include <Helios/Scene/Camera.h>
#include <Helios/Scene/Mesh.h>
#include <Helios/Scene/Material.h>
#include <Helios/VirtualDevice/DeviceTexture.h>
#include <Helios/VirtualDevice/DeviceShader.h>
#include <Helios/VirtualDevice/DeviceFrameBuffer.h>
#include <cmath>


namespace Helios
{
    ReflectionProbe::ReflectionProbe()
    {
        m_ObjectType = ObjectType::ReflectionProbe;
    }

    void ReflectionProbe::Bake(RenderView* render_view)
    {
        PROFILE_FUNCTION();

        const bool need_bake = m_IsRealtime || !m_BakeCompleted && (m_BakeConfig.BakeDiffuse || m_BakeConfig.BakeSpecular);
        if (!need_bake)
            return;

        /* 烘焙场景到立方体贴图 */
        BakeEnvCubemap(render_view);
        if (!m_BakeResult.EnvColorCubemap)
            return;

        if (m_BakeConfig.BakeDiffuse)
            BakeIrradianceMap();

        if (m_BakeConfig.BakeSpecular)
            BakePrefilterMap();

        m_BakeCompleted = true;
    }

    void ReflectionProbe::Reset()
    {
        m_BakeResult = BakeResult{};
    }

    /* 构造一个带 Color0(立方体贴图) + Depth 的离屏帧缓冲，用于把场景渲染到立方体贴图的某一面。 */
    SharedPtr<DeviceFrameBuffer> ReflectionProbe::MakeSceneCaptureFrameBuffer(const SharedPtr<DeviceTexture>& color_target, const SharedPtr<DeviceTexture>& depth_target, uint32_t size)
    {
        PROFILE_FUNCTION();

        RenderBufferInfo color_info;
        color_info.RenderTarget = color_target;
        color_info.Level = 0;
        color_info.Layer = 0;

        RenderBufferInfo depth_info;
        depth_info.RenderTarget = depth_target;
        depth_info.Level = 0;
        depth_info.Layer = 0;

        FrameBufferDesc desc;
        desc.Samples = 1;
        desc.Usage = RenderBufferUsage::ColorDefault;
        if (depth_target) desc.Usage |= RenderBufferUsage::Depth;
        desc.ViewportRegion = ViewportRegion{ 0, 0, size, size };
        desc.ColorRenderBuffers.push_back(color_info);
        desc.DepthRenderBuffer = depth_info;

        return DeviceFrameBuffer::Create(GetDebugName() + "_CaptureFrameBuffer", desc);
    }

    /* 立方体捕获相机朝向（+X, -X, +Y, -Y, +Z, -Z） */
    const static glm::mat4 GetCaptureViewMatrix(uint32_t face_index)
    {
        const glm::vec3 eye = glm::vec3(0.0f);
        switch (face_index)
        {
        case 0: return glm::lookAt(eye, glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)); // +X
        case 1: return glm::lookAt(eye, glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)); // -X
        case 2: return glm::lookAt(eye, glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)); // +Y
        case 3: return glm::lookAt(eye, glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)); // -Y
        case 4: return glm::lookAt(eye, glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, -1.0f, 0.0f)); // +Z
        case 5: return glm::lookAt(eye, glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f)); // -Z
        default: return glm::mat4(1.0f);
        }
    }

    void ReflectionProbe::BakeEnvCubemap(RenderView* render_view)
    {
        PROFILE_FUNCTION();

        const std::string label = GetDebugName() + "_BakeEnvCubemap";
        Renderer::GetRenderAPI()->PushDebugGroup(label.c_str());
        RenderQueryProfiler::Instance().BeginGPUScope(label.c_str());

        const uint32_t size = m_BakeConfig.EnvSize;
        if (!m_BakeResult.EnvColorCubemap || m_BakeResult.EnvColorCubemap->GetWidth() != size || m_BakeResult.EnvColorCubemap->GetHeight() != size)
        {
            TextureDesc env_desc;
            env_desc.SamplerType = SamplerType::SamplerCubeMap;
            env_desc.Width = size;
            env_desc.Height = size;
            env_desc.Format = TextureFormat::RGBA16F;
            env_desc.Usage = TextureUsage::ColorAttachment | TextureUsage::Sampleable;
            env_desc.MipLevels = 1;
            m_BakeResult.EnvColorCubemap = DeviceTexture::Create(GetDebugName() + "_EnvColorCubemap", env_desc);
        }

        /* 仅烘焙天空盒 */
        if (m_BakeConfig.BakeSkyBoxOnly && m_pSkyBoxTexture)
        {
            auto shader = DeviceShader::Create(ABSOLUTE_PATH("Shaders/ReflectionProbe/EquirectToCube.glsl"));
            auto material = Material::Create(shader);
            material->SetTexture("u_EquirectangularMap", m_pSkyBoxTexture);

            auto capture_fb = MakeSceneCaptureFrameBuffer(m_BakeResult.EnvColorCubemap, nullptr, size);
            for (int face = 0; face < 6; ++face)
            {
                capture_fb->Bind(FrameBufferBindInfo::ToColorLayer(0, face, 0));
                Renderer::Clear();

                material->SetParameters(ParamType::Int, "u_FaceId", face);
                Renderer::Submit(material, Renderer::GetFullScreenVertexArray());
            }
            capture_fb->Unbind();

            Renderer::GetRenderAPI()->PopDebugGroup();
            RenderQueryProfiler::Instance().EndGPUScope();
            return;
        }

        /* 烘焙场景：在探针位置用6个朝向相机实时捕获场景 */
        if (!m_BakeResult.EnvDepthCubemap || m_BakeResult.EnvDepthCubemap->GetWidth() != size || m_BakeResult.EnvDepthCubemap->GetHeight() != size)
        {
            TextureDesc depth_desc;
            depth_desc.SamplerType = SamplerType::Sampler2D;
            depth_desc.Width = size;
            depth_desc.Height = size;
            depth_desc.Format = TextureFormat::Depth24;
            depth_desc.Usage = TextureUsage::DepthAttachment;
            depth_desc.MipLevels = 1;
            m_BakeResult.EnvDepthCubemap = DeviceTexture::Create(GetDebugName() + "_EnvDepthCubemap", depth_desc);
        }

        const float aspect = 1.0f;
        const float near_plane = 0.1f;
        const float far_plane = 1000.0f;
        const glm::mat4 capture_projection = MakeReversedZProjection(glm::perspective(glm::radians(90.0f), aspect, near_plane, far_plane));
        const glm::vec3 capture_position = GetPosition();

        const auto& visible_objects = render_view->GetVisibleMeshObjects();

        auto capture_fb = MakeSceneCaptureFrameBuffer(m_BakeResult.EnvColorCubemap, m_BakeResult.EnvDepthCubemap, size);
        for (uint32_t face = 0; face < 6; ++face)
        {
            capture_fb->Bind(FrameBufferBindInfo::ToColorLayer(0, face, 0));
            Renderer::Clear();

            const glm::mat4 view = GetCaptureViewMatrix(face);
            Renderer::SetViewUniforms(view, capture_projection, capture_position);

            for (const auto& mesh_object : visible_objects)
            {
                Renderer::FillObjectUniformBuffer(mesh_object);
                auto& material = mesh_object.MeshSegment->GetMaterial();
                material->SetParameters(ParamType::Int, "u_UseIBL", 0);
                Renderer::Submit(material, mesh_object.MeshSegment->GetMeshPrimitive());
            }
        }
        capture_fb->Unbind();

        Renderer::GetRenderAPI()->PopDebugGroup();
        RenderQueryProfiler::Instance().EndGPUScope();
    }

    void ReflectionProbe::BakeIrradianceMap()
    {
        PROFILE_FUNCTION();

        const std::string label = GetDebugName() + "_BakeIrradianceMap";
        Renderer::GetRenderAPI()->PushDebugGroup(label.c_str());
        RenderQueryProfiler::Instance().BeginGPUScope(label.c_str());

        const uint32_t size = m_BakeConfig.IrradianceSize;
        if (!m_BakeResult.IrradianceMap || m_BakeResult.IrradianceMap->GetWidth() != size || m_BakeResult.IrradianceMap->GetHeight() != size)
        {
            TextureDesc irradiance_desc;
            irradiance_desc.SamplerType = SamplerType::SamplerCubeMap;
            irradiance_desc.Width = size;
            irradiance_desc.Height = size;
            irradiance_desc.Format = TextureFormat::RGBA16F;
            irradiance_desc.Usage = TextureUsage::ColorAttachment | TextureUsage::Sampleable;
            irradiance_desc.MipLevels = 1;
            m_BakeResult.IrradianceMap = DeviceTexture::Create(GetDebugName() + "_IrradianceMap", irradiance_desc);
        }

        auto shader = DeviceShader::Create(ABSOLUTE_PATH("Shaders/ReflectionProbe/Irradiance.glsl"));
        auto material = Material::Create(shader);
        material->SetTexture("u_EnvironmentMap", m_BakeResult.EnvColorCubemap);

        auto fb = MakeSceneCaptureFrameBuffer(m_BakeResult.IrradianceMap, nullptr, size);
        for (int face = 0; face < 6; ++face)
        {
            fb->Bind(FrameBufferBindInfo::ToColorLayer(0, face, 0));
            Renderer::Clear();
            material->SetParameters(ParamType::Int, "u_FaceId", face);
            Renderer::Submit(material, Renderer::GetFullScreenVertexArray());
        }
        fb->Unbind();

        Renderer::GetRenderAPI()->PopDebugGroup();
        RenderQueryProfiler::Instance().EndGPUScope();
    }

    void ReflectionProbe::BakePrefilterMap()
    {
        PROFILE_FUNCTION();

        const std::string label = GetDebugName() + "_BakePrefilterMap";
        Renderer::GetRenderAPI()->PushDebugGroup(label.c_str());
        RenderQueryProfiler::Instance().BeginGPUScope(label.c_str());

        const uint32_t size = m_BakeConfig.PrefilterSize;
        const uint32_t mip_levels = m_BakeConfig.PrefilterMipLevels > 0
            ? m_BakeConfig.PrefilterMipLevels
            : static_cast<uint32_t>(std::floor(std::log2(static_cast<float>(m_BakeConfig.PrefilterSize)))) + 1; /* 计算实际层级数 */

        if (!m_BakeResult.PrefilterMap || m_BakeResult.PrefilterMap->GetWidth() != size || m_BakeResult.PrefilterMap->GetHeight() != size
            || m_BakeResult.PrefilterMap->GetTextureDesc().MipLevels != mip_levels)
        {
            TextureDesc prefilter_desc;
            prefilter_desc.SamplerType = SamplerType::SamplerCubeMap;
            prefilter_desc.Width = size;
            prefilter_desc.Height = size;
            prefilter_desc.Format = TextureFormat::RGBA16F;
            prefilter_desc.Usage = TextureUsage::ColorAttachment | TextureUsage::Sampleable;
            prefilter_desc.MipLevels = mip_levels;
            m_BakeResult.PrefilterMap = DeviceTexture::Create(GetDebugName() + "_PrefilterMap", prefilter_desc);
        }

        auto shader = DeviceShader::Create(ABSOLUTE_PATH("Shaders/ReflectionProbe/Prefilter.glsl"));
        auto material = Material::Create(shader);
        material->SetTexture("u_EnvironmentMap", m_BakeResult.EnvColorCubemap);

        auto fb = MakeSceneCaptureFrameBuffer(m_BakeResult.PrefilterMap, nullptr, size);
        for (uint32_t mip = 0; mip < mip_levels; ++mip)
        {
            const uint32_t mip_size = static_cast<uint32_t>(size * std::pow(0.5f, static_cast<float>(mip)));
            const float roughness = static_cast<float>(mip) / static_cast<float>(mip_levels - 1);
            material->SetParameters(ParamType::Float, "u_Roughness", roughness);

            for (int face = 0; face < 6; ++face)
            {
                fb->Bind(FrameBufferBindInfo::ToColorLayer(0, face, mip));
                Renderer::SetViewport(0, 0, mip_size, mip_size);
                Renderer::Clear();

                material->SetParameters(ParamType::Int, "u_FaceId", face);
                Renderer::Submit(material, Renderer::GetFullScreenVertexArray());
            }
        }
        fb->Unbind();

        Renderer::GetRenderAPI()->PopDebugGroup();
        RenderQueryProfiler::Instance().EndGPUScope();
    }

    void ReflectionProbeManager::RegisterProbe(const SharedPtr<ReflectionProbe>& probe)
    {
        PROFILE_FUNCTION();

        if (!probe)
            return;
        if (std::find(m_RegisteredProbes.begin(), m_RegisteredProbes.end(), probe) == m_RegisteredProbes.end())
            m_RegisteredProbes.push_back(probe);
    }

    void ReflectionProbeManager::UnregisterProbe(const SharedPtr<ReflectionProbe>& probe)
    {
        PROFILE_FUNCTION();

        auto it = std::find(m_RegisteredProbes.begin(), m_RegisteredProbes.end(), probe);
        if (it != m_RegisteredProbes.end())
            m_RegisteredProbes.erase(it);
    }

    /* 获取距离最近的反射探针 */
    SharedPtr<ReflectionProbe> ReflectionProbeManager::GetClostedReflectionProbe(const glm::vec3& target_pos) const
    {
        PROFILE_FUNCTION();

        float best_dist_sq = std::numeric_limits<float>::max();
        SharedPtr<ReflectionProbe> chosen = nullptr;

        for (const auto& probe : m_RegisteredProbes)
        {
            if (!probe || !probe->IsBaked())
                continue;
            const float dist_sq = glm::distance2(target_pos, probe->GetPosition());
            if (dist_sq < best_dist_sq)
            {
                best_dist_sq = dist_sq;
                chosen = probe;
            }
        }

        return chosen;
    }

    void ReflectionProbeManager::Prepare()
    {
        PROFILE_FUNCTION();

        m_NeedBakeProbes.clear();

        /* 从已注册的反射探针中，筛选需要烘焙的（realtime 探针每帧烘焙，未烘焙的 dirty 探针需要烘焙） */
        for (const auto& probe : m_RegisteredProbes)
        {
            if (!probe || !probe->GetEnable())
                continue;
            if (probe->IsRealtime() || !probe->IsBaked())
                m_NeedBakeProbes.push_back(probe);
        }
    }

    void ReflectionProbeManager::AddBakeReflectionProbePass(RenderView* render_view)
    {
        PROFILE_FUNCTION();

        if (!render_view || m_NeedBakeProbes.empty())
            return;

        auto& frame_graph = render_view->GetFrameGraph();
        if (!frame_graph)
            return;

        /* 遍历当前需要烘焙的反射探针 */
        struct BakeProbePassData {};
        for (const auto& probe : m_NeedBakeProbes)
        {
            if (!probe)
                continue;

            frame_graph->AddPass<BakeProbePassData>("BakeReflectionProbePass",
                [&](FrameGraphBuilder& builder, BakeProbePassData&)
                {
                    builder.AsSideEffect(true);
                },
                [probe, render_view](const FrameGraphResources& resources, const BakeProbePassData&)
                {
                    probe->Bake(render_view);
                });
        }

        /* BRDFLut 是通用查找表: 只需烘焙一次 */
        if (m_BRDFLutMap) return;
        BakeBRDFLutMap();
    }

    /* 烘焙BRDF查找表 */
    void ReflectionProbeManager::BakeBRDFLutMap()
    {
        PROFILE_FUNCTION();

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
            auto shader = DeviceShader::Create(ABSOLUTE_PATH("Shaders/ReflectionProbe/BRDFLut.glsl"));
            auto material = Material::Create(shader);

            Renderer::Clear();
            Renderer::Submit(material, MeshPrimitive(Renderer::GetFullScreenVertexArray()));
        }
        fb->Unbind();
    }
}
