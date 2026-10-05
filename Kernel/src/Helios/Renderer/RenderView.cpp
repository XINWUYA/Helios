#include "Pch.h"
#include "RenderView.h"
#include "FrameGraph/FrameGraph.h"
#include <Helios/Scene/Scene.h>
#include <Helios/Scene/Components.h>
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

		return DynamicPtrCast<Resource<FrameGraphTexture>>(m_pFrameGraph->GetResource(m_RenderTargetHandle))->GetResource().Texture;
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

		/* 每帧重算级联阴影的视图投影矩阵与分割距离，使方向光旋转 / 相机移动实时反映到阴影投影
		 * （矩阵计算收口于 ShadowMapManager）。传入可见网格：光源视锥拟合需纳入投射物包围盒。 */
		if (m_IsHasShadowCast)
			m_pShadowMapManager->UpdateCascadeMatrices(GetCullingCamera(), &m_VisibleMeshObjects);

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
			auto [transform_component, model_component] = model_entity_view.get<TransformComponent, ModelComponent>(entity);
			if (!model_component.m_Model)
				 continue;

			for (const auto& mesh_segment : model_component.m_Model->GetMeshSegments())
			{
				const auto& world_position = transform_component.m_Position;
				// todo: 执行剔除
				//if ()
				m_VisibleMeshObjects.emplace_back((int)entity, transform_component.GetTransform(), mesh_segment);
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
			auto [transform_component, light_component] = light_entity_view.get<TransformComponent, LightComponent>(entity);
			if (light_component.m_Light)
			{
				m_ValidLights.emplace_back(light_component.m_Light);

				if (light_component.m_Light->IsCastShadow())
				{
					m_pShadowMapManager->RegisterShadowLight(light_component.m_Light);
					m_IsHasShadowCast = true;
				}
			}
		}

		if (m_IsHasShadowCast)
			m_pShadowMapManager->PrepareForShadowMaps(owner_scene, GetCullingCamera());
	}

	/* 更新FrameGraph */
	void RenderView::UpdateFrameGraph()
	{
		PROFILE_FUNCTION();

		/* 重置FrameGraph */
		m_pFrameGraph->Reset();

        auto scene = m_pOwnerScene.lock();

		/* 相机可以自行组织渲染图（如编辑器相机用延迟渲染图，以便按像素拾取实体）。
		 * 未自行组织的回落到默认的前向渲染图。 */
		const bool organized_by_camera = (m_pOwnerCamera != nullptr) && m_pOwnerCamera->ConstructRenderView(*this);
		if (!organized_by_camera)
		{
			/* 根据是否有光源开启ShadowCast来注入ShadowPass */
			if (m_IsHasShadowCast)
				m_pShadowMapManager->AddShadowPass(*m_pFrameGraph, scene, this);

			/* 烘焙ReflectionProbe */
			auto probe_manager = scene->GetReflectionProbeManager();
			if (probe_manager)
			{
				probe_manager->Prepare();
				probe_manager->AddBakeReflectionProbePass(this);
			}

			/* ScenePass */
			Forward::AddScenePass(this, m_IsHasShadowCast);
		}

		/* 生成当前FrameGraph */
		m_pFrameGraph->Build();

		// m_pFrameGraph->ExportGraphviz("framegraph.txt");
	}

}
