#include "Pch.h"
#include "ReflectionProbe.h"
#include "SceneCommon.h"
#include "Helios/Renderer/FrameGraph/FrameGraph.h"
#include <Helios/Renderer/RenderView.h>
#include <Helios/Renderer/Renderer.h>
#include <Helios/Renderer/RenderCommon.h>
#include <Helios/Renderer/RenderPasses/ScenePass.h>
#include <Helios/Renderer/RenderPasses/TerrainPasses.h>
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

        const uint32_t size = m_BakeConfig.EnvSize;
        if (!m_BakeResult.EnvColorCubemap || m_BakeResult.EnvColorCubemap->GetWidth() != size || m_BakeResult.EnvColorCubemap->GetHeight() != size)
        {
            TextureDesc env_desc;
            env_desc.SamplerType = SamplerType::SamplerCubeMap;
            env_desc.Width = size;
            env_desc.Height = size;
            env_desc.Format = TextureFormat::RGBA16F;
            env_desc.Usage = TextureUsage::ColorAttachment | TextureUsage::Sampleable;
            /* 完整 mip 链：预滤波按采样立体角选 LOD 时需要更模糊的源层级，
             * 只渲染 mip0 会让极亮小特征（太阳）走样成孤立白斑 */
            env_desc.MipLevels = static_cast<uint8_t>(std::floor(std::log2(static_cast<float>(size)))) + 1;
            m_BakeResult.EnvColorCubemap = DeviceTexture::Create(GetDebugName() + "_EnvColorCubemap", env_desc);
        }

        /* 直接用作者的天空贴图生成环境（贴图为空时退到下面的场景捕获） */
        if (m_BakeConfig.BakeFromSkyTexture && m_pSkyBoxTexture)
        {
            /* 走资产缓存而不是直建实例：同一路径拿到同一实例，热重载才覆盖得到 */
            auto shader = AssetManager::Instance().GetOrLoad<DeviceShader>(
                ABSOLUTE_PATH("Shaders/ReflectionProbe/EquirectToCube.glsl"));
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

        /* 本探针只捕获自己关心的层：按 CullingMask 重新收集一遍，
         * 不受触发烘焙的那个相机看了哪些层的限制。收集口径与相机渲染同源，天空也在里面。 */
        std::vector<VisibleMeshObject> visible_objects;
        render_view->CollectProbeCaptureObjects(GetCullingMask(), visible_objects);

        /* 几何走管线自己的提交：排序、逐光源合成、透明批都跟相机渲染一致。
         * 捕获阶段关掉探针 IBL —— 否则会把上一次的烘焙结果烘进新结果里。 */
        const auto scene = render_view->GetOwnerScene();
        Forward::SubmitParams submit_params;
        submit_params.Objects = &visible_objects;
        submit_params.SortOrigin = &capture_position;
        submit_params.IgnoreProbes = true;

        auto capture_fb = MakeSceneCaptureFrameBuffer(m_BakeResult.EnvColorCubemap, m_BakeResult.EnvDepthCubemap, size);
        for (uint32_t face = 0; face < 6; ++face)
        {
            capture_fb->Bind(FrameBufferBindInfo::ToColorLayer(0, face, 0));
            Renderer::Clear();

            const glm::mat4 view = GetCaptureViewMatrix(face);
            /* 捕获相机在探针位置：视图 = 朝向 × 平移(-探针位置)（标准 lookAt 约定
             * R·T(-eye)）——只给朝向矩阵会让捕获发生在世界原点，探针离原点越远、
             * 烘焙出的环境越离谱。 */
            Renderer::SetViewUniforms(
                view * glm::translate(glm::mat4(1.0f), -capture_position),
                capture_projection, capture_position);

            if (scene != nullptr)
            {
                Forward::SubmitSceneObjects(*render_view, *scene, false, submit_params);
                Forward::SubmitSceneObjects(*render_view, *scene, true, submit_params);

                /* 地形同样不在可见对象表里：不补这一趟，反射里就没有地面 */
                TerrainDraw::SubmitCapture(*render_view, *scene, capture_position, far_plane);
            }
        }
        capture_fb->Unbind();

        /* 捕获过程中改写了全局共享的 View 常量（上面每面都调用 SetViewUniforms 换成
         * 捕获相机）。ViewUniformBuffer 是全局唯一的，若不恢复，本 Pass 之后的
         * ScenePass 会继续用最后一个捕获面的视角（探针位置、90° FOV）绘制主视图。 */
        if (render_view)
        {
            if (const Camera* culling_camera = render_view->GetCullingCamera())
            {
                Renderer::SetViewUniforms(culling_camera->GetViewMatrix(),
                    culling_camera->GetProjectionMatrix(), culling_camera->GetPosition());
            }
        }

        Renderer::GetRenderAPI()->PopDebugGroup();
    }

    void ReflectionProbe::BakeIrradianceMapFace(uint32_t face)
    {
        PROFILE_FUNCTION();

        const std::string label = GetDebugName() + "_BakeIrradianceMap";
        Renderer::GetRenderAPI()->PushDebugGroup(label.c_str());

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

        auto shader = AssetManager::Instance().GetOrLoad<DeviceShader>(
            ABSOLUTE_PATH("Shaders/ReflectionProbe/Irradiance.glsl"));
        auto material = Material::Create(shader);
        material->SetTexture("u_EnvironmentMap", m_BakeResult.EnvColorCubemap);

        /* 源环境立方图的最大 mip（= log2(size)）：卷积据此换算出源分辨率，
         * 再按每次采样的立体角选源 mip，避免点采样命中太阳这类极亮 texel。 */
        const uint32_t env_mip_levels = m_BakeResult.EnvColorCubemap
            ? m_BakeResult.EnvColorCubemap->GetTextureDesc().MipLevels
            : 1u;
        material->SetParameters(ParamType::Float, "u_EnvironmentMapMaxMip",
            static_cast<float>(env_mip_levels > 0 ? env_mip_levels - 1u : 0u));

        auto fb = MakeSceneCaptureFrameBuffer(m_BakeResult.IrradianceMap, nullptr, size);
        fb->Bind(FrameBufferBindInfo::ToColorLayer(0, face, 0));
        Renderer::Clear();
        material->SetParameters(ParamType::Int, "u_FaceId", static_cast<int>(face));
        Renderer::Submit(material, Renderer::GetFullScreenVertexArray());
        fb->Unbind();

        Renderer::GetRenderAPI()->PopDebugGroup();
    }

    void ReflectionProbe::BakePrefilterMapFace(uint32_t face)
    {
        PROFILE_FUNCTION();

        const std::string label = GetDebugName() + "_BakePrefilterMap";
        Renderer::GetRenderAPI()->PushDebugGroup(label.c_str());

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

        auto shader = AssetManager::Instance().GetOrLoad<DeviceShader>(
            ABSOLUTE_PATH("Shaders/ReflectionProbe/Prefilter.glsl"));
        auto material = Material::Create(shader);
        material->SetTexture("u_EnvironmentMap", m_BakeResult.EnvColorCubemap);

        /* 源环境立方图的最大 mip：预滤波按 u_Roughness * MaxMip 选取采样层级。 */
        const uint32_t env_mip_levels = m_BakeResult.EnvColorCubemap
            ? m_BakeResult.EnvColorCubemap->GetTextureDesc().MipLevels
            : 1u;
        material->SetParameters(ParamType::Float, "u_EnvironmentMapMaxMip",
            static_cast<float>(env_mip_levels > 0 ? env_mip_levels - 1u : 0u));

        auto fb = MakeSceneCaptureFrameBuffer(m_BakeResult.PrefilterMap, nullptr, size);
        for (uint32_t mip = 0; mip < mip_levels; ++mip)
        {
            const uint32_t mip_size = static_cast<uint32_t>(size * std::pow(0.5f, static_cast<float>(mip)));
            const float roughness = static_cast<float>(mip) / static_cast<float>(mip_levels - 1);
            material->SetParameters(ParamType::Float, "u_Roughness", roughness);

            fb->Bind(FrameBufferBindInfo::ToColorLayer(0, face, mip));
            Renderer::SetViewport(0, 0, mip_size, mip_size);
            Renderer::Clear();

            material->SetParameters(ParamType::Int, "u_FaceId", static_cast<int>(face));
            Renderer::Submit(material, Renderer::GetFullScreenVertexArray());
        }
        fb->Unbind();

        Renderer::GetRenderAPI()->PopDebugGroup();
    }
}
