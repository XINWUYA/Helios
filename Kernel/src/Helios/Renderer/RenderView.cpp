#include "Pch.h"
#include "RenderView.h"
#include "FrameGraph/FrameGraph.h"
#include <Helios/VirtualDevice/DeviceTexture.h>
#include <Helios/Scene/MeshInstanceIndex.h>
#include <Helios/Scene/Scene.h>
#include <Helios/Scene/Camera.h>
#include <Helios/Scene/Material.h>
#include <Helios/Scene/Model.h>
#include <Helios/Core/AssetManager.h>
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
	Frustum Frustum::FromViewProjection(const glm::mat4& view_projection)
	{
		/* glm 按列存，第 i 行 = (m[0][i], m[1][i], m[2][i], m[3][i]) */
		const auto row = [&view_projection](int index)
		{
			return glm::vec4(view_projection[0][index], view_projection[1][index],
				view_projection[2][index], view_projection[3][index]);
		};
		const glm::vec4 row_x = row(0);
		const glm::vec4 row_y = row(1);
		const glm::vec4 row_z = row(2);
		const glm::vec4 row_w = row(3);
		/* 侧面 |x| <= w、|y| <= w；深度按 ZO 的 0 <= z <= w（Reversed-Z 只是近远两端对调，
		 * 围出来的还是这块体积），所以两个深度面一起加上不用管约定 */
		Frustum frustum;
		frustum.Planes[0] = row_w + row_x;
		frustum.Planes[1] = row_w - row_x;
		frustum.Planes[2] = row_w + row_y;
		frustum.Planes[3] = row_w - row_y;
		frustum.Planes[4] = row_z;
		frustum.Planes[5] = row_w - row_z;
		frustum.ViewProjection = view_projection;
		return frustum;
	}

	bool Frustum::IsOutside(const VisibleMeshObject& object) const
	{
		if (!object.Cullable)
			return false;

		glm::vec3 center = object.WorldCenter;
		glm::vec3 extent = object.WorldExtent;
		if (!object.HasWorldBounds)
		{
			/* 没在收集时量过（手工构造的对象）：现算一次，局部盒变到世界 ——
			 * 中心直接变换，半长按矩阵各轴的绝对值展开 */
			if (object.MeshSegment == nullptr)
				return false;

			const glm::vec3& local_min = object.MeshSegment->GetAABBMin();
			const glm::vec3& local_max = object.MeshSegment->GetAABBMax();
			/* 没设过包围盒（全零）当作未知，不剔 */
			if (local_min == glm::vec3(0.0f) && local_max == glm::vec3(0.0f))
				return false;

			const glm::mat4& matrix = object.Local2WorldMat;
			const glm::vec3 half = (local_max - local_min) * 0.5f;
			center = glm::vec3(matrix * glm::vec4((local_min + local_max) * 0.5f, 1.0f));
			extent = glm::abs(glm::vec3(matrix[0])) * half.x
				+ glm::abs(glm::vec3(matrix[1])) * half.y
				+ glm::abs(glm::vec3(matrix[2])) * half.z;
		}

		return IsOutside(center, extent);
	}

	bool Frustum::IsOutside(const glm::vec3& center, const glm::vec3& extent) const
	{
		for (const glm::vec4& plane : Planes)
		{
			/* 平面没归一化也没关系：只看符号，支撑函数 n·c + |n|·e 照样对 */
			const glm::vec3 normal(plane);
			if (glm::dot(normal, center) + plane.w + glm::dot(glm::abs(normal), extent) < 0.0f)
				return true;
		}
		return false;
	}

	void Frustum::GetWorldBounds(glm::vec3& bounds_min, glm::vec3& bounds_max) const
	{
		const glm::mat4 inverse = glm::inverse(ViewProjection);
		bounds_min = glm::vec3(std::numeric_limits<float>::max());
		bounds_max = glm::vec3(-std::numeric_limits<float>::max());

		/* 两个深度端点都取：Reversed-Z 下 0 是远、1 是近，八个角点合起来是同一块体积 */
		for (int depth = 0; depth <= 1; ++depth)
		{
			for (int y = -1; y <= 1; y += 2)
			{
				for (int x = -1; x <= 1; x += 2)
				{
					const glm::vec4 clip = inverse * glm::vec4(static_cast<float>(x), static_cast<float>(y),
						static_cast<float>(depth), 1.0f);
					if (std::abs(clip.w) < 1e-6f)
						continue;

					const glm::vec3 world = glm::vec3(clip) / clip.w;
					bounds_min = glm::min(bounds_min, world);
					bounds_max = glm::max(bounds_max, world);
				}
			}
		}
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

		/* 补上一帧的世界矩阵：运动矢量靠它区分"相机在动"和"物体自己在动" */
		PreparePreviousTransforms();

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

	/* 按实体把网格段组装成可绘制对象：材质解析、世界包围盒都在这里定下来 */
	void RenderView::BuildMeshObjects(const Scene& scene, const std::vector<entt::entity>& entities,
		std::vector<VisibleMeshObject>& out) const
	{
		PROFILE_FUNCTION();

		out.reserve(out.size() + entities.size());
		for (const entt::entity entity : entities)
		{
			if (!scene.IsEntityValid(entity))
				continue;

			const auto* model_component = scene.GetRegistry().try_get<ModelComponent>(entity);
			if (model_component == nullptr || !model_component->m_Model)
				continue;

			/* 组件里存的是相对父节点的局部变换，画到世界里的必须是沿父链累积的结果 */
			const glm::mat4 local_2_world = scene.GetRenderWorldTransform(entity);

			/* 开了蒙皮画当前姿势的动态段；关掉就回到绑定姿态的源段（口径见 Model::GetRenderSegments） */
			const auto& mesh_segments = model_component->m_EnableSkinning
				? model_component->m_Model->GetRenderSegments()
				: model_component->m_Model->GetMeshSegments();
			const bool skinned = model_component->m_EnableSkinning && !model_component->m_Model->IsStaticModel();

			for (const auto& mesh_segment : mesh_segments)
			{
				/* 收集时按解析链定下最终材质：实体绑定 ?: 模型槽表默认 ?: 内置白模 */
				const auto material = ResolveSlotMaterial(
					*model_component->m_Model, mesh_segment->GetSlotIndex(), &model_component->m_SlotOverrides);
				auto& object = out.emplace_back((int)entity, local_2_world, mesh_segment, material,
					model_component->m_CastShadow, model_component->m_ReceiveShadow);
				object.Cullable = !skinned;

				/* 世界包围盒量一次：相机视锥 / 各级联 / 点光每个面都拿它做判定 */
				const glm::vec3& local_min = mesh_segment->GetAABBMin();
				const glm::vec3& local_max = mesh_segment->GetAABBMax();
				if (local_min != glm::vec3(0.0f) || local_max != glm::vec3(0.0f))
				{
					const glm::vec3 half = (local_max - local_min) * 0.5f;
					object.WorldCenter = glm::vec3(local_2_world * glm::vec4((local_min + local_max) * 0.5f, 1.0f));
					object.WorldExtent = glm::abs(glm::vec3(local_2_world[0])) * half.x
						+ glm::abs(glm::vec3(local_2_world[1])) * half.y
						+ glm::abs(glm::vec3(local_2_world[2])) * half.z;
					object.HasWorldBounds = true;
				}
			}
		}
	}

	/* 全量收集（没有空间索引可用时的兜底：没走 OnUpdate 的检查程序、新建的空场景） */
	void RenderView::CollectVisibleMeshObjects(LayerMask culling_mask, std::vector<VisibleMeshObject>& out) const
	{
		PROFILE_FUNCTION();

		const auto owner_scene = m_pOwnerScene.lock();
		if (!owner_scene)
			return;

		std::vector<entt::entity> entities;
		const auto model_entity_view = owner_scene->GetRegistry().view<TransformComponent, ModelComponent>();
		entities.reserve(model_entity_view.size_hint());
		for (const auto& entity : model_entity_view)
		{
			/* 隐藏的实体（自身或祖先）与看不到的层不进列表：场景视口 / 阴影 /
			 * 探针烘焙都从这里取对象，一处过滤三处同步（判定收口在 Scene::IsEntityInView）。 */
			if (owner_scene->IsEntityInRenderView(entity, culling_mask))
				entities.push_back(entity);
		}

		BuildMeshObjects(*owner_scene, entities, out);
	}

	namespace
	{
		/* 阴影能影响到的世界范围（相机为中心）：
		 * 方向光按 ShadowFar 的两倍铺 —— 影子从投射物那里打过来，投射物可以比接收面更远；
		 * 点光 / 聚光按光源范围加光源到相机的距离。没有投影光源时返回 false。 */
		bool ComputeShadowBounds(const Scene& scene, const Camera* camera, glm::vec3& bounds_min, glm::vec3& bounds_max)
		{
			if (camera == nullptr)
				return false;

			const glm::vec3 center = camera->GetPosition();
			float radius = 0.0f;
			bool found = false;

			for (auto [entity, light_component] : scene.GetRegistry().view<LightComponent>().each())
			{
				const auto& light = light_component.m_Light;
				if (light == nullptr || !light->IsCastShadow())
					continue;

				found = true;
				if (light->GetLightType() == LightType::Directional)
				{
					const auto& shadow_info = light->GetShadowMapInfo();
					const float reach = (shadow_info != nullptr && shadow_info->ShadowFar > 0.0f)
						? shadow_info->ShadowFar : camera->GetFarClip();
					radius = std::max(radius, reach * 2.0f);
				}
				else if (const auto punctual = std::dynamic_pointer_cast<PunctualLight>(light))
				{
					radius = std::max(radius, punctual->GetRange() + glm::distance(punctual->GetPosition(), center));
				}
			}

			if (!found)
				return false;

			bounds_min = center - glm::vec3(radius);
			bounds_max = center + glm::vec3(radius);
			return true;
		}
	}

	namespace
	{
		/* 内建天空：单位球 + Atmosphere.glsl 材质。顶点着色器不吃模型矩阵、
		 * 把球面折成全屏方向（等价于相机永远在球心），所以整场共用一份网格与材质。
		 * 对象编号留空 —— 天空不参与拾取 */
		void AppendBuiltinSky(std::vector<VisibleMeshObject>& objects)
		{
			static const SharedPtr<Model> s_model = Model::Create(BuiltinModelType::Sphere);
			static const SharedPtr<Material> s_material = []
			{
				static BuiltinAsset<DeviceShader> s_shader("Shaders/Sky/Atmosphere.glsl");
				auto material = Material::Create(s_shader.Get());

				/* 从球心往外看，可见的是内表面：要剔正面。深度恒在远平面 */
				RenderRasterState raster_state;
				raster_state.CullMode = CullMode::Cull_Front;
				raster_state.DepthCompareFunc = CompareFunc::GreaterEqual;
				material->SetRasterState(raster_state);
				return material;
			}();

			if (s_model == nullptr || s_material == nullptr)
				return;

			for (const auto& mesh_segment : s_model->GetMeshSegments())
				objects.emplace_back(-1, glm::mat4(1.0f), mesh_segment, s_material, false, false);
		}

		/* 已经摆过天空对象就别再补：作者选的材质优先（可以换成环境贴图） */
		bool HasSkyObject(const std::vector<VisibleMeshObject>& objects)
		{
			for (const VisibleMeshObject& object : objects)
			{
				if (object.Material != nullptr && object.Material->IsSkyBox())
					return true;
			}
			return false;
		}
	}

	/* 探针捕获的对象表：按层收集，天空按与主视图同一套口径补（内建球）。
	 * 探针只看自己那几层，场景里的天空往往只是组件、没有网格，所以必须在这里补。 */
	void RenderView::CollectProbeCaptureObjects(LayerMask culling_mask, std::vector<VisibleMeshObject>& out) const
	{
		CollectVisibleMeshObjects(culling_mask, out);

		const auto owner_scene = m_pOwnerScene.lock();
		if (owner_scene && owner_scene->HasActiveSky() && !HasSkyObject(out))
			AppendBuiltinSky(out);
	}

	/* 视锥体剔除：空间索引按视锥查候选，再逐个组装成可绘制对象 */
	void RenderView::PrepareVisibleObjects()
	{
		PROFILE_FUNCTION();

		m_SceneMeshObjects.clear();
		m_VisibleMeshObjects.clear();

		const auto owner_scene = m_pOwnerScene.lock();
		if (!owner_scene)
			return;

		const Camera* camera = GetCullingCamera();
		const MeshInstanceIndex& index = owner_scene->GetMeshInstanceIndex();

		/* 索引还没同步（没走 OnUpdate）：退回全量扫描，行为与以前一致 */
		if (index.IsEmpty())
		{
			CollectVisibleMeshObjects(m_CullingMask, m_SceneMeshObjects);
			if (!m_IsEnableCulling || camera == nullptr)
			{
				m_VisibleMeshObjects = m_SceneMeshObjects;
				if (owner_scene->HasActiveSky() && !HasSkyObject(m_VisibleMeshObjects))
					AppendBuiltinSky(m_VisibleMeshObjects);
				return;
			}

			const Frustum frustum = Frustum::FromViewProjection(camera->GetViewProjectionMatrix());
			m_VisibleMeshObjects.reserve(m_SceneMeshObjects.size());
			for (const VisibleMeshObject& object : m_SceneMeshObjects)
			{
				const bool is_sky = object.Material != nullptr && object.Material->IsSkyBox();
				if (is_sky || !frustum.IsOutside(object))
					m_VisibleMeshObjects.push_back(object);
			}
			if (owner_scene->HasActiveSky() && !HasSkyObject(m_VisibleMeshObjects))
				AppendBuiltinSky(m_VisibleMeshObjects);
			return;
		}

		/* 查询范围 = 视锥 ∪ 阴影范围：一次查询、一次组装，再按各自口径分拣。
		 * 两趟查询会把同一批对象组装两遍（材质解析才是大头），三万个对象时差出一倍 */
		Frustum frustum;
		const bool cull = m_IsEnableCulling && camera != nullptr;
		bool has_query = false;
		glm::vec3 query_min(0.0f);
		glm::vec3 query_max(0.0f);

		if (cull)
		{
			frustum = Frustum::FromViewProjection(camera->GetViewProjectionMatrix());
			frustum.GetWorldBounds(query_min, query_max);
			has_query = true;
		}

		glm::vec3 shadow_min(0.0f);
		glm::vec3 shadow_max(0.0f);
		const bool has_shadow = ComputeShadowBounds(*owner_scene, camera, shadow_min, shadow_max);
		if (has_shadow)
		{
			query_min = has_query ? glm::min(query_min, shadow_min) : shadow_min;
			query_max = has_query ? glm::max(query_max, shadow_max) : shadow_max;
			has_query = true;
		}

		std::vector<entt::entity> candidates;
		if (has_query)
			index.QueryBounds(query_min, query_max, m_CullingMask, candidates);
		else
			index.QueryAll(m_CullingMask, candidates);

		std::vector<VisibleMeshObject> built;
		BuildMeshObjects(*owner_scene, candidates, built);

		/* 分拣：投射物按阴影范围，可见按视锥（天空恒可见；没有包围盒的对象两边都收）。
		 * 阴影范围盖住整个查询范围时（大阴影距离的常见情形）候选全是投射物，直接整份接管 */
		const bool all_casters = has_shadow && has_query
			&& shadow_min.x <= query_min.x && shadow_min.y <= query_min.y && shadow_min.z <= query_min.z
			&& shadow_max.x >= query_max.x && shadow_max.y >= query_max.y && shadow_max.z >= query_max.z;

		if (!has_shadow)
		{
			/* 没有投影光源：投射物列表留空 */
		}
		else if (all_casters)
		{
			m_SceneMeshObjects = built;
		}
		else
		{
			for (const VisibleMeshObject& object : built)
			{
				if (!object.HasWorldBounds
					|| (object.WorldCenter.x + object.WorldExtent.x >= shadow_min.x
						&& object.WorldCenter.x - object.WorldExtent.x <= shadow_max.x
						&& object.WorldCenter.y + object.WorldExtent.y >= shadow_min.y
						&& object.WorldCenter.y - object.WorldExtent.y <= shadow_max.y
						&& object.WorldCenter.z + object.WorldExtent.z >= shadow_min.z
						&& object.WorldCenter.z - object.WorldExtent.z <= shadow_max.z))
					m_SceneMeshObjects.push_back(object);
			}
		}

		if (!cull)
		{
			m_VisibleMeshObjects = std::move(built);
		}
		else
		{
			m_VisibleMeshObjects.reserve(built.size());
			for (const VisibleMeshObject& object : built)
			{
				if ((object.Material != nullptr && object.Material->IsSkyBox())
					|| !object.HasWorldBounds || !frustum.IsOutside(object))
					m_VisibleMeshObjects.push_back(object);
			}
		}

		/* 天空盒不进空间索引：不看网格位置，按层补进可见列表 */
		candidates.clear();
		for (const entt::entity entity : owner_scene->GetSkyBoxEntities())
		{
			if (Layers::IsVisible(owner_scene->GetEntityLayerMask(entity), m_CullingMask))
				candidates.push_back(entity);
		}
		/* 这一趟只往列表里补天空对象：加进来几条，就是作者摆了几个 */
		const size_t object_count_before_sky = m_VisibleMeshObjects.size();
		BuildMeshObjects(*owner_scene, candidates, m_VisibleMeshObjects);

		/* 场景只挂了天空组件、没摆天空网格时补一个内建天空（见 AppendBuiltinSky）。
		 * 判据用「这一趟补进来几条」而不是回头扫可见列表（几万个对象的热路径），
		 * 也不是「有没有别的网格」（那会让真实场景里一直缺天） */
		if (owner_scene->HasActiveSky() && m_VisibleMeshObjects.size() == object_count_before_sky)
			AppendBuiltinSky(m_VisibleMeshObjects);
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

		/* 方向光的"实际贡献"：颜色里已经带了天空算出的透过率（太阳落下地平线就是 0），
		 * 再乘强度就是它这一刻能不能照到东西 */
		const auto direction_contribution = [](const Light& light)
		{
			if (light.GetLightType() != LightType::Directional)
				return 0.0f;
			const glm::vec4& color = light.GetColor();
			return (0.299f * color.r + 0.587f * color.g + 0.114f * color.b) * light.GetIntensity();
		};

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

				if (!light_component.m_Light->IsCastShadow())
					continue;

				/* 照不到东西的方向光不占级联层：太阳落到地平线以下、月亮还没升起来的那一段，
				 * 它俩的颜色 / 强度都是 0，再按它的方向拟合阴影只会在画面里留下错误的投影 */
				if (direction_contribution(*light_component.m_Light) <= 0.0f
					&& light_component.m_Light->GetLightType() == LightType::Directional)
					continue;

				shadow_lights.emplace_back(light_component.m_Light);
			}
		}

		/* 级联阴影只有一套，管理器按注册顺序认第一盏方向光当宿主，所以要把真正在照明的那盏
		 * 排在前面：太阳落下、月亮接班时顺序会翻，阴影才不会挂在一盏照不到东西的光上 */
		std::stable_sort(shadow_lights.begin(), shadow_lights.end(),
			[&direction_contribution](const SharedPtr<Light>& left, const SharedPtr<Light>& right)
			{
				return direction_contribution(*left) > direction_contribution(*right);
			});

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

	/* 给本帧可见对象补上一帧的世界矩阵：TAA 没开就都填当前值（速度 0），开了才维护跨帧表。
	 * 一个模型的多个网格段共享同一个矩阵，按 ObjectId 存一份就够 */
	void RenderView::PreparePreviousTransforms()
	{
		PROFILE_FUNCTION();

		if (!m_IsTemporalEnabled)
		{
			for (VisibleMeshObject& object : m_VisibleMeshObjects)
				object.PrevLocal2WorldMat = object.Local2WorldMat;
			m_PrevWorldTransforms.clear();
			m_CurrentWorldTransforms.clear();
			m_PrevTransformFrame = 0;
			return;
		}

		/* 换了新的一帧才把上一帧记下的那份翻上来当基准。同一帧被渲染第二次
		 * （面板重绘、抓图）时不能翻，否则"上一帧"就成了本帧，速度全变 0 */
		const auto owner_scene = m_pOwnerScene.lock();
		const uint32_t frame_index = owner_scene != nullptr ? owner_scene->GetFrameIndex() : 0;
		const bool new_frame = frame_index != m_PrevTransformFrame;
		if (new_frame)
		{
			m_PrevWorldTransforms.swap(m_CurrentWorldTransforms);
			m_PrevTransformFrame = frame_index;
		}

		for (VisibleMeshObject& object : m_VisibleMeshObjects)
		{
			const auto it = m_PrevWorldTransforms.find(object.ObjectId);
			/* 新出现的对象没有上一帧，速度按 0 算（拿当前值当上一帧） */
			object.PrevLocal2WorldMat = (it != m_PrevWorldTransforms.end())
				? it->second : object.Local2WorldMat;
		}

		if (!new_frame)
			return;

		m_CurrentWorldTransforms.clear();
		m_CurrentWorldTransforms.reserve(m_VisibleMeshObjects.size());
		for (const VisibleMeshObject& object : m_VisibleMeshObjects)
			m_CurrentWorldTransforms[object.ObjectId] = object.Local2WorldMat;
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

	namespace
	{
		/* Halton(2,3) 前 16 项，减 0.5 落到 [-0.5, 0.5)，单位是像素。
		 * 项数就是收敛时间：再多几项细节略好，残影也更久。 */
		const glm::vec2 kJitterSequence[16] =
		{
			{  0.0000f, -0.1667f }, { -0.2500f,  0.1667f }, {  0.2500f, -0.3889f }, { -0.3750f, -0.0556f },
			{  0.1250f,  0.2778f }, { -0.1250f, -0.2778f }, {  0.3750f,  0.0556f }, { -0.4375f,  0.3889f },
			{  0.0625f, -0.4630f }, { -0.1875f, -0.1296f }, {  0.3125f,  0.2037f }, { -0.3125f, -0.3519f },
			{  0.1875f, -0.0185f }, { -0.0625f,  0.3148f }, {  0.4375f, -0.2407f }, { -0.4688f,  0.0926f },
		};
	}

	void RenderView::BeginTemporalFrame(bool jitter_enabled)
	{
		PROFILE_FUNCTION();

		m_IsTemporalEnabled = jitter_enabled && m_pOwnerCamera != nullptr;

		glm::vec2 jitter(0.0f);
		if (jitter_enabled && m_pOwnerCamera != nullptr
			&& m_ViewportRegion.Width > 0 && m_ViewportRegion.Height > 0)
		{
			jitter = kJitterSequence[m_JitterIndex % 16];
			m_JitterIndex = (m_JitterIndex + 1) % 16;
		}
		else
		{
			/* 关掉时相位归零、历史作废：下次打开的第一帧必须吃当前帧 */
			m_JitterIndex = 0;
			m_IsHistoryValid = false;
		}
		m_JitterPixels = jitter;

		if (m_pOwnerCamera == nullptr)
		{
			m_RenderProjection = glm::mat4(1.0f);
			m_RenderViewProjection = glm::mat4(1.0f);
			m_VelocityViewProjection = glm::mat4(1.0f);
			m_PrevVelocityViewProjection = glm::mat4(1.0f);
			return;
		}

		/* 上一帧的渲染矩阵先留下来：本帧的抖动与相机都变了，算完再取就没有"上一帧"了。
		 * 第 1 帧这里是单位矩阵，速度会很大，但历史本来就无效，当帧输出不受影响 */
		const glm::mat4 previous_view_projection = m_RenderViewProjection;
		const glm::mat4 previous_velocity_view_projection = m_VelocityViewProjection;

		/* 亚像素偏移加在投影矩阵的 [2][0] / [2][1]：透视下 NDC 偏移 = -m[2][0]，
		 * 所以取负号才是"右移几像素" */
		m_RenderProjection = m_pOwnerCamera->GetProjectionMatrix();
		if (jitter != glm::vec2(0.0f))
		{
			m_RenderProjection[2][0] = -2.0f * jitter.x / static_cast<float>(m_ViewportRegion.Width);
			m_RenderProjection[2][1] = -2.0f * jitter.y / static_cast<float>(m_ViewportRegion.Height);
		}
		m_RenderViewProjection = m_RenderProjection * m_pOwnerCamera->GetViewMatrix();
		/* 运动矢量的基准不带抖动：抖动是同一表面点换了个像素，算进位移里会让
		 * 每个像素每帧都"在动"，历史被采到邻像素，反倒更抖 */
		m_VelocityViewProjection = m_pOwnerCamera->GetViewProjectionMatrix();

		/* 相机瞬移判定：上一帧远平面的四个角，用本帧矩阵投回去，屏幕位移超过两屏就当成不连续。
		 * 正常飞行时这四个角基本不动，只有真正的跳变才会触发 */
		if (m_IsHistoryValid && jitter_enabled)
		{
			const glm::mat4 inverse_prev = glm::inverse(previous_view_projection);
			float max_shift = 0.0f;
			for (int corner_y = -1; corner_y <= 1 && max_shift <= 2.0f; corner_y += 2)
			{
				for (int corner_x = -1; corner_x <= 1 && max_shift <= 2.0f; corner_x += 2)
				{
					const glm::vec4 world = inverse_prev * glm::vec4(
						static_cast<float>(corner_x), static_cast<float>(corner_y), 0.0f, 1.0f);
					const glm::vec4 clip = m_RenderViewProjection * (world / world.w);
					if (clip.w <= 1.0e-6f)
					{
						max_shift = 1.0e9f;
						break;
					}
					const glm::vec2 ndc = glm::vec2(clip) / clip.w;
					max_shift = std::max(max_shift,
						glm::length(ndc - glm::vec2(static_cast<float>(corner_x), static_cast<float>(corner_y))));
				}
			}
			if (max_shift > 2.0f)
				m_IsHistoryValid = false;
		}

		/* 本帧运动矢量用的上一帧基准 */
		m_PrevVelocityViewProjection = previous_velocity_view_projection;
	}

	SharedPtr<DeviceTexture> RenderView::AcquireHistory(const std::string& key, uint32_t width, uint32_t height,
		TextureFormat format)
	{
		PROFILE_FUNCTION();

		HistoryTexture& entry = m_HistoryTextures[key];
		if (entry.Texture != nullptr && entry.Width == width && entry.Height == height && entry.Format == format)
			return entry.Texture;

		TextureDesc desc;
		desc.Width = width;
		desc.Height = height;
		desc.Format = format;
		desc.Usage = TextureUsage::ColorAttachment | TextureUsage::Sampleable;
		entry.Texture = DeviceTexture::Create("History_" + key, desc);
		entry.Width = width;
		entry.Height = height;
		entry.Format = format;

		/* 尺寸或格式变了，旧内容对不上新画面 */
		m_IsHistoryValid = false;
		return entry.Texture;
	}

	/* 更新FrameGraph */
	void RenderView::UpdateFrameGraph()	{
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
