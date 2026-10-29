#include "Pch.h"
#include "RenderView.h"
#include "FrameGraph/FrameGraph.h"
#include <Helios/Scene/Scene.h>
#include <Helios/Scene/Camera.h>
#include <Helios/Scene/Material.h>
#include <Helios/Scene/Components.h>
#include <Helios/Scene/Mesh.h>
#include <Helios/Scene/ShadowMap.h>
#include <Helios/Scene/Light.h>
#include <Helios/Scene/ReflectionProbe.h>
#include <Helios/Scene/SkeletonModel.h>
#include <Helios/Renderer/RenderPasses/DeferredPasses.h>
#include <Helios/Renderer/RenderPasses/ScenePass.h>

namespace Helios
{
	FrustumSides FrustumSides::FromViewProjection(const glm::mat4& view_projection)
	{
		/* glm 按列存，第 i 行 = (m[0][i], m[1][i], m[2][i], m[3][i]) */
		const auto row = [&view_projection](int index)
		{
			return glm::vec4(view_projection[0][index], view_projection[1][index],
				view_projection[2][index], view_projection[3][index]);
		};
		const glm::vec4 row_x = row(0);
		const glm::vec4 row_y = row(1);
		const glm::vec4 row_w = row(3);
		return FrustumSides{ { row_w + row_x, row_w - row_x, row_w + row_y, row_w - row_y } };
	}

	bool FrustumSides::IsOutside(const VisibleMeshObject& object) const
	{
		if (!object.Cullable || object.MeshSegment == nullptr)
			return false;

		const glm::vec3& local_min = object.MeshSegment->GetAABBMin();
		const glm::vec3& local_max = object.MeshSegment->GetAABBMax();
		/* 没设过包围盒（全零）当作未知，不剔 */
		if (local_min == glm::vec3(0.0f) && local_max == glm::vec3(0.0f))
			return false;

		/* 局部盒变到世界：中心直接变换，半长按矩阵各轴的绝对值展开 */
		const glm::mat4& matrix = object.Local2WorldMat;
		const glm::vec3 half = (local_max - local_min) * 0.5f;
		const glm::vec3 center = glm::vec3(matrix * glm::vec4((local_min + local_max) * 0.5f, 1.0f));
		const glm::vec3 extent = glm::abs(glm::vec3(matrix[0])) * half.x
			+ glm::abs(glm::vec3(matrix[1])) * half.y
			+ glm::abs(glm::vec3(matrix[2])) * half.z;

		for (const glm::vec4& plane : Planes)
		{
			const glm::vec3 normal(plane);
			if (glm::dot(normal, center) + plane.w + glm::dot(glm::abs(normal), extent) < 0.0f)
				return true;
		}
		return false;
	}

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

		m_SceneMeshObjects.clear();
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

		/* 缺失时返回静态空值：直接 `return {}` 会悬垂（临时对象），
		 * 查询不存在的 Pass 名即崩 */
		static const SharedPtr<DeviceFrameBuffer> s_EmptyFrameBuffer{};
		return s_EmptyFrameBuffer;
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

		/* 每帧重算阴影矩阵：级联随方向光旋转 / 相机移动实时更新（传入剔除前的投射物：
		 * 光源视锥拟合需纳入投射物包围盒）；点光/聚光的矩阵随处 → 范围变化即更新。 */
		if (m_IsHasShadowCast)
		{
			m_pShadowMapManager->UpdateCascadeMatrices(GetCullingCamera(), &m_SceneMeshObjects);
			m_pShadowMapManager->UpdatePunctualMatrices();
		}

		m_pFrameGraph->Execute();
	}

	/* 视锥体剔除 */
	void RenderView::PrepareVisibleObjects()
	{
		PROFILE_FUNCTION();

		m_SceneMeshObjects.clear();
		m_VisibleMeshObjects.clear();
		CollectVisibleMeshObjects(m_CullingMask, m_SceneMeshObjects);

		const Camera* camera = GetCullingCamera();
		if (!m_IsEnableCulling || camera == nullptr)
		{
			m_VisibleMeshObjects = m_SceneMeshObjects;
			return;
		}

		/* 天空盒铺满全屏，跟自己的网格摆在哪无关，恒画 */
		const FrustumSides frustum = FrustumSides::FromViewProjection(camera->GetViewProjectionMatrix());
		m_VisibleMeshObjects.reserve(m_SceneMeshObjects.size());
		for (const VisibleMeshObject& object : m_SceneMeshObjects)
		{
			const bool is_sky = object.Material != nullptr && object.Material->IsSkyBox();
			if (is_sky || !frustum.IsOutside(object))
				m_VisibleMeshObjects.push_back(object);
		}
	}

	void RenderView::CollectVisibleMeshObjects(LayerMask culling_mask, std::vector<VisibleMeshObject>& out) const
	{
		PROFILE_FUNCTION();

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

			/* 隐藏的实体（自身或祖先）与看不到的层不进列表：场景视口 / 阴影 /
			 * 探针烘焙都从这里取对象，一处过滤三处同步（判定收口在 Scene::IsEntityInView）。 */
			if (!owner_scene->IsEntityInRenderView(entity, culling_mask))
				continue;

			/* 组件里存的是相对父节点的局部变换，画到世界里的必须是沿父链累积的结果 */
			const glm::mat4 local_2_world = owner_scene->GetRenderWorldTransform(entity);

			/* 开了蒙皮画当前姿势的动态段；关掉就回到绑定姿态的源段（口径见 Model::GetRenderSegments） */
			const auto& mesh_segments = model_component.m_EnableSkinning
				? model_component.m_Model->GetRenderSegments()
				: model_component.m_Model->GetMeshSegments();
			const bool skinned = model_component.m_EnableSkinning && !model_component.m_Model->IsStaticModel();

			for (const auto& mesh_segment : mesh_segments)
			{
				// todo: 执行剔除
				//if ()
				/* 收集时按解析链定下最终材质：实体绑定 ?: 模型槽表默认 ?: 内置白模 */
				const auto material = ResolveSlotMaterial(
					*model_component.m_Model, mesh_segment->GetSlotIndex(), &model_component.m_SlotOverrides);
				auto& object = out.emplace_back((int)entity, local_2_world, mesh_segment, material,
					model_component.m_CastShadow, model_component.m_ReceiveShadow);
				object.Cullable = !skinned;
			}
		}
	}

	namespace
	{
		/* 阴影光源的签名：指针、类型、阴影图尺寸、级联数 —— 任一项变了才重建阴影管理器 */
		uint64_t ShadowLightSignature(const Light& light)
		{
			uint64_t signature = 0xCBF29CE484222325ULL;
			const auto mix = [&signature](uint64_t value)
			{
				signature ^= value;
				signature *= 0x100000001B3ULL;
			};
			mix(reinterpret_cast<uintptr_t>(&light));
			mix(static_cast<uint64_t>(light.GetLightType()));
			if (const auto& info = light.GetShadowMapInfo())
			{
				mix(info->Size);
				mix(info->CascadeCnt);
			}
			return signature;
		}
	}

	/* 准备光源信息 */
	void RenderView::PrepareLights()
	{
		PROFILE_FUNCTION();

		m_ValidLights.clear();
		m_IsHasShadowCast = false;

		/* 收集所有光源 */
		const auto owner_scene = m_pOwnerScene.lock();
		if (!owner_scene)
			return;

		std::vector<SharedPtr<Light>> shadow_lights;

		const auto light_entity_view = owner_scene->GetRegistry().view<TransformComponent, LightComponent>();
		for (auto& entity : light_entity_view)
		{
			/* 隐藏的光源不参与光照（与模型的过滤同源：隐藏 = 不画 + 不照）；
			 * 层同理 —— 相机看不到的层，它的光源也不进本视图 */
			if (!owner_scene->IsEntityInRenderView(entity, m_CullingMask))
				continue;

			auto [transform_component, light_component] = light_entity_view.get<TransformComponent, LightComponent>(entity);
			if (light_component.m_Light)
			{
				m_ValidLights.emplace_back(light_component.m_Light);

				if (light_component.m_Light->IsCastShadow())
					shadow_lights.emplace_back(light_component.m_Light);
			}
		}

		/* 投影光源是同一批就沿用上一帧的注册结果（级联矩阵的脏检测另有方向 / 相机比较） */
		std::vector<uint64_t> signatures;
		signatures.reserve(shadow_lights.size());
		for (const auto& light : shadow_lights)
			signatures.push_back(ShadowLightSignature(*light));
		if (signatures != m_ShadowLightSignatures)
		{
			m_pShadowMapManager->Reset();
			for (const auto& light : shadow_lights)
				m_pShadowMapManager->RegisterShadowLight(light);
			m_ShadowLights = shadow_lights;
			m_ShadowLightSignatures = std::move(signatures);
			m_pShadowMapManager->PrepareForShadowMaps(owner_scene, GetCullingCamera());
		}

		m_IsHasShadowCast = !m_pShadowMapManager->GetCascadeShadowMaps().empty()
			|| !m_pShadowMapManager->GetPunctualShadowMaps().empty();
	}

	namespace
	{
		/* 场景级维护：推进探针烘焙状态机并挂接烘焙 Pass —— 与渲染管线选择无关，两种管线共用。
		 * 烘焙要用中性阴影图，渲染图里没建阴影 Pass 时只推进状态机。 */
		void PrepareReflectionProbeBaking(RenderView& render_view, const SharedPtr<Scene>& scene)
		{
			auto probe_manager = scene->GetReflectionProbeManager();
			if (probe_manager)
			{
				probe_manager->Prepare();
				if (render_view.GetFrameGraph()->GetBlackboard().GetResourceHandle<FrameGraphTexture>("NoShadowMapHandle"))
					probe_manager->AddBakeReflectionProbePass(&render_view);
			}
		}
	}

	/* 更新FrameGraph */
	void RenderView::UpdateFrameGraph()
	{
		PROFILE_FUNCTION();

		/* 重置FrameGraph */
		m_pFrameGraph->Reset();

		/* GPU 蒙皮排在最前：这一帧改过姿势的骨骼顶点先算出来，
		 * 后面的阴影 / 场景 / 延迟 Pass 拿到的都是新顶点 */
		SkeletonModel::AddGpuSkinPass(*this);

		/* 渲染图已销毁，旧的输出句柄随之失效。
		 * 重置它，未重新指定输出的渲染图（如相机在视口尺寸无效时不组织渲染图）不会读到悬垂资源。 */
		m_RenderTargetHandle.Reset();

        auto scene = m_pOwnerScene.lock();

		/* 相机可以自行组织渲染图（如编辑器相机：按管线分派 + 拾取通道 + gizmo 叠加）。
		 * 未自行组织的按本视图的管线选择组织默认渲染图 —— 前向 / 延迟两种都从这里走。 */
		const bool organized_by_camera = (m_pOwnerCamera != nullptr) && m_pOwnerCamera->ConstructRenderView(*this);
		if (!organized_by_camera)
		{
			if (m_RenderPipeline == RenderPipeline::Deferred)
			{
				/* 延迟：阴影前置 / 材质转换 / 探针选择都在模块内；输出（光照结果）为视图纹理 */
				Deferred::AddDeferredPasses(this);
			}
			else
			{
				/* 前向：Probe 捕获使用无阴影图；ScenePass 有级联阴影时使用当前视图的阴影图。 */
				AddShadowMapPasses();

				if (m_IsOutputToTexture)
					Forward::AddScenePassToTexture(this);
				else
					Forward::AddScenePass(this);
			}
		}

		/* 探针烘焙挂在管线之后，结果从下一帧起被场景 / 光照采样；
		 * 自己组织渲染图的相机（编辑器相机）也可能被选中来推进 */
		if (m_IsBakeReflectionProbes && scene)
			PrepareReflectionProbeBaking(*this, scene);

		/* 视图输出在帧后被界面读取，标成导出 */
		m_pFrameGraph->ExportResource(m_RenderTargetHandle);

		/* 生成当前FrameGraph */
		m_pFrameGraph->Build();

		// m_pFrameGraph->ExportGraphviz("framegraph.txt");
	}

}
