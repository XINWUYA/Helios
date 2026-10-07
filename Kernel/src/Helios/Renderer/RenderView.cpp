#include "Pch.h"
#include "RenderView.h"
#include "FrameGraph/FrameGraph.h"
#include <Helios/Scene/Scene.h>
#include <Helios/Scene/Components.h>
#include <Helios/Scene/Mesh.h>
#include <Helios/Scene/ShadowMap.h>
#include <Helios/Scene/Light.h>
#include <Helios/Scene/ReflectionProbe.h>
#include <Helios/Renderer/RenderPasses/ScenePass.h>

namespace Helios
{
	RenderView::RenderView(Camera* owner_camera)
		: m_pOwnerCamera(owner_camera)
	{
		PROFILE_FUNCTION();

		m_pFrameGraph = CreateSharedPtr<FrameGraph>(m_pOwnerCamera->GetDebugName() + "_FrameGraph");
		m_pShadowMapManager = CreateSharedPtr<ShadowMapManager>();
	}

	RenderView::~RenderView()
	{
		PROFILE_FUNCTION();

		m_VisibleMeshObjects.clear();
	}

	/* 设置视口区域 */
	void RenderView::SetViewportRegion(const ViewportRegion& region)
	{
		PROFILE_FUNCTION();

		ASSERT(region.Width > 0 && region.Height > 0, "ViewprotRegion's width or height is invaild.");
		m_ViewportRegion = region;
	}

	SharedPtr<DeviceTexture> RenderView::GetRenderTarget() const
	{
		PROFILE_FUNCTION();

		if (!m_RenderTargetHandle.IsInitialized())
			return nullptr;

		const auto& resource = DynamicPtrCast<Resource<FrameGraphTexture>>(m_pFrameGraph->GetResource(m_RenderTargetHandle));
		return (resource != nullptr) ? resource->GetResource().Texture : nullptr;
	}

	/* 存储各Pass的FrameBuffer */
	void RenderView::EmplacePassFrameBuffer(const std::string& name, const SharedPtr<DeviceFrameBuffer>& frame_buffer)
	{
		PROFILE_FUNCTION();

		m_PassFrameBuffers[name] = frame_buffer;
	}

	const SharedPtr<DeviceFrameBuffer>& RenderView::GetPassFrameBuffer(const std::string& name) const
	{
		PROFILE_FUNCTION();

		const auto it = m_PassFrameBuffers.find(name);
		if (it != m_PassFrameBuffers.end())
			return it->second;
		return {};
	}

	/* 为本视图的渲染图准备阴影：中性深度数组恒建（保证任何着色阶段都能采样到合法纹理），
	 * 本视图有投影光源时再生成真实阴影图；句柄都落进 Blackboard 的 "ShadowMapHandle"。 */
	void RenderView::AddShadowMapPasses()
	{
		PROFILE_FUNCTION();

		m_pShadowMapManager->AddNoShadowMapPass(*m_pFrameGraph);
		if (m_IsHasShadowCast)
		{
			m_pShadowMapManager->AddShadowPass(*m_pFrameGraph, GetOwnerScene(), this);
		}
		else
		{
			const auto fallback_handle = m_pFrameGraph->GetBlackboard()
				.GetResourceHandle<FrameGraphTexture>("NoShadowMapHandle");
			m_pFrameGraph->GetBlackboard()["ShadowMapHandle"] = fallback_handle;
		}
	}

	/* 准备一帧的RenderView数据 */
	void RenderView::Prepare()
	{
		PROFILE_FUNCTION();

		
	}

	/* 执行渲染当前View */
	void RenderView::Execute()
	{
		PROFILE_FUNCTION();

		const auto owner_scene = m_pOwnerScene.lock();
		if (!owner_scene)
		{
			CORE_LOG_ERROR("Invalid scene for RenderView.");
			return;
		}

		/* 收集当前RenderView可见的对象 */
		PrepareVisibleObjects();

		/* 收集光源信息并准备阴影（含纹理描述与VP矩阵） */
		PrepareLights();

		UpdateFrameGraph();

		/* 每帧重算阴影矩阵：级联随方向光旋转 / 相机移动实时更新（传入可见网格：
		 * 光源视锥拟合需纳入投射物包围盒）；点光/聚光的矩阵随处 → 范围变化即更新。 */
		if (m_IsHasShadowCast)
		{
			m_pShadowMapManager->UpdateCascadeMatrices(GetCullingCamera(), &m_VisibleMeshObjects);
			m_pShadowMapManager->UpdatePunctualMatrices();
		}

		m_pFrameGraph->Execute();
	}

	/* 视锥体剔除 */
	void RenderView::PrepareVisibleObjects()
	{
		PROFILE_FUNCTION();

		m_VisibleMeshObjects.clear();

		// todo: 视锥体剔除

		/* 收集所有模型 */
		const auto owner_scene = m_pOwnerScene.lock();
		if (!owner_scene)
			return;

		const auto model_entity_view = owner_scene->GetRegistry().view<TransformComponent, ModelComponent>();
		for (auto& entity : model_entity_view)
		{
			const auto& model_component = model_entity_view.get<ModelComponent>(entity);
			if (!model_component.m_Model)
				 continue;

			/* 隐藏的实体（自身或祖先）不进可见列表：场景视口 / 阴影 / 探针烘焙
			 * 都从这里取对象，一处过滤三处同步（判定收口在 Scene::IsEntityVisible）。 */
			if (!owner_scene->IsEntityVisible(entity))
				continue;

			/* 组件里存的是相对父节点的局部变换，画到世界里的必须是沿父链累积的结果 */
			const glm::mat4 local_2_world = owner_scene->GetWorldTransform(entity);

			for (const auto& mesh_segment : model_component.m_Model->GetMeshSegments())
			{
				// todo: 执行剔除
				//if ()
				/* 收集时按解析链定下最终材质：实体绑定 ?: 模型槽表默认 ?: 内置白模 */
				const auto material = ResolveSlotMaterial(
					*model_component.m_Model, mesh_segment->GetSlotIndex(), &model_component.m_SlotOverrides);
				m_VisibleMeshObjects.emplace_back((int)entity, local_2_world, mesh_segment, material,
					model_component.m_CastShadow, model_component.m_ReceiveShadow);
			}
		}

	}

	/* 准备光源信息 */
	void RenderView::PrepareLights()
	{
		PROFILE_FUNCTION();

		m_ValidLights.clear();
		m_IsHasShadowCast = false;
		m_pShadowMapManager->Reset();

		/* 收集所有光源 */
		const auto owner_scene = m_pOwnerScene.lock();
		if (!owner_scene)
			return;

		const auto light_entity_view = owner_scene->GetRegistry().view<TransformComponent, LightComponent>();
		for (auto& entity : light_entity_view)
		{
			/* 隐藏的光源不参与光照（与模型的过滤同源：隐藏 = 不画 + 不照） */
			if (!owner_scene->IsEntityVisible(entity))
				continue;

			auto [transform_component, light_component] = light_entity_view.get<TransformComponent, LightComponent>(entity);
			if (light_component.m_Light)
			{
				m_ValidLights.emplace_back(light_component.m_Light);

				if (light_component.m_Light->IsCastShadow())
					m_pShadowMapManager->RegisterShadowLight(light_component.m_Light);
			}
		}

		m_IsHasShadowCast = !m_pShadowMapManager->GetCascadeShadowMaps().empty()
			|| !m_pShadowMapManager->GetPunctualShadowMaps().empty();
		if (m_IsHasShadowCast)
			m_pShadowMapManager->PrepareForShadowMaps(owner_scene, GetCullingCamera());
	}

	/* 更新FrameGraph */
	void RenderView::UpdateFrameGraph()
	{
		PROFILE_FUNCTION();

		/* 重置FrameGraph */
		m_pFrameGraph->Reset();

		/* 渲染图已销毁，旧的输出句柄随之失效。
		 * 重置它，未重新指定输出的渲染图（如相机在视口尺寸无效时不组织渲染图）不会读到悬垂资源。 */
		m_RenderTargetHandle.Reset();

        auto scene = m_pOwnerScene.lock();

		/* 相机可以自行组织渲染图（如编辑器相机用延迟渲染图，以便按像素拾取实体）。
		 * 未自行组织的回落到默认的前向渲染图。 */
		const bool organized_by_camera = (m_pOwnerCamera != nullptr) && m_pOwnerCamera->ConstructRenderView(*this);
		if (!organized_by_camera)
		{
			/* Probe 捕获使用无阴影图；ScenePass 有级联阴影时使用当前视图的阴影图。 */
			AddShadowMapPasses();

			/* 烘焙ReflectionProbe */
			auto probe_manager = scene->GetReflectionProbeManager();
			if (probe_manager)
			{
				probe_manager->Prepare();
				probe_manager->AddBakeReflectionProbePass(this);
			}

			/* ScenePass */
			Forward::AddScenePass(this);
		}

		/* 生成当前FrameGraph */
		m_pFrameGraph->Build();

		// m_pFrameGraph->ExportGraphviz("framegraph.txt");
	}

}
