#include "Pch.h"
#include "Scene.h"
#include "SkeletonModel.h"
#include <tinyxml2.h>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include "Components.h"
#include "Entity.h"
#include "Material.h"
#include "ShadowMap.h"
#include "Sky.h"
#include "Helios/Common/Utils.h"
#include "Helios/Common/Math.h"
#include "Camera.h"
#include "Helios/Renderer/Renderer.h"
#include "Helios/Renderer/RenderView.h"
#include "Helios/Renderer/RenderPasses/GrassDrawParams.h"
#include "Helios/Renderer/PostProcess/PostProcessEffectRegistry.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include "Helios/Script/ScriptRuntime.h"
#include "SceneCommon.h"

namespace Helios
{
	namespace
	{
		/* 父链深度上限：兜住畸形场景文件里可能出现的环（正常层级远小于此） */
		constexpr size_t kMaxHierarchyDepth = 64;

		/* 视锥平面归一化：着色器按 dot(n, c) + w + r 判球，不用每个块再算一次 length() */
		glm::vec4 NormalizePlane(const glm::vec4& plane)
		{
			const float normal_length = glm::length(glm::vec3(plane));
			return normal_length > 1e-6f ? plane / normal_length : plane;
		}
	}

	Scene::Scene()
	{
		m_pReflectionProbeManager = CreateSharedPtr<ReflectionProbeManager>();
		m_pScriptRuntime = CreateUniquePtr<ScriptRuntime>();

		/* 统一连接所有组件类型的信号 */
        ConnectSignalsForComponents(SceneComponentList{});
	}

	Scene::~Scene()
	{
		m_RenderViews.clear();

		/* 先断开所有组件的信号连接 */
		DisconnectSignalsForComponents(SceneComponentList{});
		ClearAllEntities();
	}

	/* 创建一个实体 */
	Entity Scene::CreateEntity(const std::string& name)
	{
		return CreateEntityInternal(name, entt::null);
	}

	/* preserve_handle 非空时优先用它：entt 在槽位空闲时原样返回该句柄，
	 * 被占则新分配一个（克隆的源场景句柄必然唯一，占位只可能来自坏文件） */
	Entity Scene::CreateEntityInternal(const std::string& name, entt::entity preserve_handle)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

		entt::entity handle = m_Registry.create(preserve_handle);
		if (preserve_handle != entt::null && handle != preserve_handle)
			CORE_LOG_WARN("Entity handle {} is taken, falling back to {}.", entt::to_integral(preserve_handle),
				entt::to_integral(handle));

		Entity entity = { handle, shared_from_this() };

		/* 默认添加名称组件和变换组件 */
		entity.AddComponent<NameComponent>(name.empty() ? "New Entity" : name);
		entity.AddComponent<TransformComponent>();

		return entity;
	}

	/* 销毁实体 */
	void Scene::QueueDestroy(Entity entity)
	{
		if (!entity || !m_Registry.valid(static_cast<entt::entity>(entity)))
			return;

		const entt::entity handle = static_cast<entt::entity>(entity);
		if (std::find(m_PendingDestroy.begin(), m_PendingDestroy.end(), handle) == m_PendingDestroy.end())
			m_PendingDestroy.push_back(handle);
	}

	/* 脚本请求的销毁排到本帧末尾处理：此时脚本 tick 与各趟视图遍历都已结束，
	 * registry 进入稳定状态，删实体不会打断迭代。 */
	void Scene::FlushPendingDestroy()
	{
		if (m_PendingDestroy.empty())
			return;

		const std::vector<entt::entity> pending = std::move(m_PendingDestroy);
		m_PendingDestroy.clear();

		for (const entt::entity handle : pending)
		{
			if (!m_Registry.valid(handle))
				continue;

			/* 先停掉挂在上面的脚本实例：OnDestroy 里还能访问实体 */
			if (m_pScriptRuntime != nullptr)
				m_pScriptRuntime->DestroyInstancesFor(handle);

			Entity entity{ handle, shared_from_this() };
			DestroyEntity(entity);
		}
	}

	void Scene::DestroyEntity(Entity& entity)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

		/* 骨骼模型：先把它的关节实体一起清掉，别在场景里留孤儿 */
		SkeletonModel::Clear(*this, static_cast<entt::entity>(entity));

		/* 子节点先脱离并保持世界位置：父实体的句柄马上会失效，
		 * 子节点若继续指向它就会从层级树里消失，空间位置也会跟着跳变。 */
		std::vector<entt::entity> children;
		const auto parent_view = m_Registry.view<ParentComponent>();
		for (const entt::entity candidate : parent_view)
		{
			if (parent_view.get<ParentComponent>(candidate).m_Parent == static_cast<entt::entity>(entity))
				children.emplace_back(candidate);
		}

		/* 提升层级会改写 ParentComponent（结构变更），必须与遍历分开做 */
		for (const entt::entity child : children)
			SetParent(child, entt::null);

		m_Registry.destroy(entity);
	}

	entt::entity Scene::GetParent(entt::entity entity) const
	{
		if (!m_Registry.valid(entity))
			return entt::null;

		const auto* parent_component = m_Registry.try_get<ParentComponent>(entity);
		if (parent_component == nullptr || !m_Registry.valid(parent_component->m_Parent))
			return entt::null;

		return parent_component->m_Parent;
	}

	bool Scene::IsAncestor(entt::entity ancestor, entt::entity entity) const
	{
		if (ancestor == entt::null)
			return false;

		/* 沿父链上溯；深度上限兜住畸形场景文件里可能出现的环 */
		entt::entity current = entity;
		for (size_t depth = 0; current != entt::null && depth < kMaxHierarchyDepth; ++depth)
		{
			if (current == ancestor)
				return true;

			current = GetParent(current);
		}
		return false;
	}

	/* 实体是否参与渲染：自身与全部祖先都可见（隐藏沿父链传染）。
	 * 与 IsAncestor / GetWorldTransform 同一套上溯写法：深度上限兜住畸形层级。 */
	bool Scene::IsEntityVisible(entt::entity entity) const
	{
		entt::entity current = entity;
		for (size_t depth = 0; current != entt::null && depth < kMaxHierarchyDepth; ++depth)
		{
			if (!m_Registry.valid(current))
				return false;

			if (const auto* visibility = m_Registry.try_get<VisibilityComponent>(current);
				visibility != nullptr && !visibility->m_Visible)
				return false;

			current = GetParent(current);
		}

		/* 句柄无效（或父链异常长）时不参与渲染 */
		return current == entt::null;
	}

	LayerMask Scene::GetEntityLayerMask(entt::entity entity) const
	{
		if (!m_Registry.valid(entity))
			return Layers::None;

		const auto* layer = m_Registry.try_get<LayerComponent>(entity);
		return (layer != nullptr) ? layer->m_Mask : Layers::Default;
	}

	bool Scene::IsEntityInView(entt::entity entity, LayerMask culling_mask) const
	{
		return IsEntityVisible(entity) && Layers::IsVisible(GetEntityLayerMask(entity), culling_mask);
	}

	glm::mat4 Scene::GetWorldTransform(entt::entity entity) const
	{
		/* 先自下而上收集父链，再自顶向下累积：一趟 O(深度)，不需要递归 */
		entt::entity chain[kMaxHierarchyDepth];
		size_t depth = 0;

		entt::entity current = entity;
		while (current != entt::null && depth < kMaxHierarchyDepth && m_Registry.valid(current))
		{
			chain[depth++] = current;
			current = GetParent(current);
		}

		glm::mat4 world{ 1.0f };
		while (depth > 0)
		{
			if (const auto* transform_component = m_Registry.try_get<TransformComponent>(chain[--depth]))
				world *= transform_component->GetTransform();
		}
		return world;
	}

	const glm::mat4& Scene::FrameWorldTransform(entt::entity entity)
	{
		/* 往上找到第一个本帧算过的祖先（或根），再自顶向下逐个补齐并记下 */
		entt::entity chain[kMaxHierarchyDepth];
		size_t depth = 0;
		glm::mat4 world{ 1.0f };
		bool visible = true;
		bool reached_top = false;

		entt::entity current = entity;
		while (current != entt::null && depth < kMaxHierarchyDepth && m_Registry.valid(current))
		{
			const size_t index = static_cast<size_t>(entt::to_entity(current));
			if (index < m_FrameRecords.size() && m_FrameRecords[index].Stamp == m_FrameWorldStampNow)
			{
				world = m_FrameRecords[index].World;
				visible = m_FrameRecords[index].Visible;
				reached_top = true;
				break;
			}
			chain[depth++] = current;
			current = GetParent(current);
		}
		/* 父链异常长（畸形层级）按不可见处理，与 IsEntityVisible 同口径 */
		if (!reached_top && current != entt::null)
			visible = false;

		while (depth > 0)
		{
			const entt::entity node = chain[--depth];
			const size_t index = static_cast<size_t>(entt::to_entity(node));
			if (index >= m_FrameRecords.size())
				m_FrameRecords.resize(index + 1);

			FrameRecord& record = m_FrameRecords[index];
			record.Entity = node;
			record.Stamp = m_FrameWorldStampNow;

			if (const auto* transform_component = m_Registry.try_get<TransformComponent>(node))
			{
				/* 局部矩阵按位置 / 旋转 / 缩放的值缓存：没动过的实体不再重算三角函数 */
				const bool local_changed = !record.HasLocal
					|| record.LocalPosition != transform_component->m_Position
					|| record.LocalRotation != transform_component->m_Rotation
					|| record.LocalScale != transform_component->m_Scale;
				if (local_changed)
				{
					record.LocalPosition = transform_component->m_Position;
					record.LocalRotation = transform_component->m_Rotation;
					record.LocalScale = transform_component->m_Scale;
					record.LocalMatrix = transform_component->GetTransform();
					record.HasLocal = true;
				}

				/* 父世界与上次相同就不再做矩阵乘：静止场景整条链都走这条捷径 */
				if (local_changed || !record.HasWorld || !record.HasParentWorld || record.ParentWorld != world)
				{
					record.World = world * record.LocalMatrix;
					record.ParentWorld = world;
					record.HasParentWorld = true;
					record.HasWorld = true;
				}
				world = record.World;
			}
			else
			{
				/* 没有变换组件的节点：世界变换就是父世界（与全量现算的口径一致） */
				record.World = world;
				record.HasWorld = true;
			}
			if (const auto* visibility = m_Registry.try_get<VisibilityComponent>(node); visibility != nullptr && !visibility->m_Visible)
				visible = false;

			record.Visible = visible;
		}

		const size_t index = static_cast<size_t>(entt::to_entity(entity));
		static const glm::mat4 s_Identity{ 1.0f };
		return (index < m_FrameRecords.size() && m_FrameRecords[index].Stamp == m_FrameWorldStampNow)
			? m_FrameRecords[index].World : s_Identity;
	}

	bool Scene::HasFrameRecord(entt::entity entity) const
	{
		const size_t index = static_cast<size_t>(entt::to_entity(entity));
		if (!m_FrameRecordReadable || !m_Registry.valid(entity) || index >= m_FrameRecords.size())
			return false;

		const FrameRecord& record = m_FrameRecords[index];
		return record.Stamp == m_FrameWorldStampNow && record.Entity == entity;
	}

	glm::mat4 Scene::GetRenderWorldTransform(entt::entity entity) const
	{
		if (HasFrameRecord(entity))
			return m_FrameRecords[static_cast<size_t>(entt::to_entity(entity))].World;
		return GetWorldTransform(entity);
	}

	bool Scene::IsEntityInRenderView(entt::entity entity, LayerMask culling_mask) const
	{
		if (!HasFrameRecord(entity))
			return IsEntityInView(entity, culling_mask);
		return m_FrameRecords[static_cast<size_t>(entt::to_entity(entity))].Visible
			&& Layers::IsVisible(GetEntityLayerMask(entity), culling_mask);
	}

	bool Scene::SetParent(entt::entity child, entt::entity parent)
	{
		if (!m_Registry.valid(child))
			return false;

		/* child 是 parent 自身或祖先：挂上去会成环（IsAncestor 含自身，覆盖了自挂） */
		if (IsAncestor(child, parent))
			return false;

		const glm::mat4 world = GetWorldTransform(child);
		const glm::mat4 parent_world = GetWorldTransform(parent);

		SetParentLink(child, parent);

		/* 换父空间后本地变换重算：世界变换保持不变，挂上去不会跳位置 */
		if (auto* transform_component = m_Registry.try_get<TransformComponent>(child))
		{
			TransformComponent reparented = *transform_component;
			if (DecomposeTransform(glm::inverse(parent_world) * world,
				reparented.m_Position, reparented.m_Rotation, reparented.m_Scale))
			{
				*transform_component = reparented;
			}
		}
		return true;
	}

	void Scene::SetParentLink(entt::entity child, entt::entity parent)
	{
		if (!m_Registry.valid(child))
			return;

		/* 回到根层级就不留组件：没有父节点的实体不该带着这条记录 */
		if (parent == entt::null || !m_Registry.valid(parent))
		{
			m_Registry.remove<ParentComponent>(child);
			return;
		}

		m_Registry.emplace_or_replace<ParentComponent>(child, parent);
	}

	void Scene::ClearAllEntities()
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

		/* 实体即将销毁，动画会话与脚本实例都不再有宿主 */
		m_AnimationRuntime.Clear();
		if (m_pScriptRuntime != nullptr)
			m_pScriptRuntime->Stop();

		/* 实体销毁会触发组件的 OnRemoved（反射探针随之从管理器注销），
		 * 但管理器仍可能持有引用，一并清空以保证不留上一份场景的残留。 */
		m_Registry.clear();
		m_MeshIndex.Clear();
		m_FrameRecords.clear();

		if (m_pReflectionProbeManager != nullptr)
			m_pReflectionProbeManager->ClearProbes();
	}

	/* 外部相机登记：不属于场景、但要渲染本场景的相机（编辑器视口 / 工具预览等）。
	 * 重复登记无效果；生命周期由调用方保证（相机销毁前先摘除）。 */
	void Scene::AddExternalCamera(Camera* camera)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

		if (camera == nullptr)
			return;
		if (std::find(m_ExternalCameras.begin(), m_ExternalCameras.end(), camera) != m_ExternalCameras.end())
			return;
		m_ExternalCameras.push_back(camera);
	}

	/* 外部相机摘除：不在册时无效果 */
	void Scene::RemoveExternalCamera(Camera* camera)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

		m_ExternalCameras.erase(
			std::remove(m_ExternalCameras.begin(), m_ExternalCameras.end(), camera),
			m_ExternalCameras.end());
	}

	ScriptRuntime& Scene::GetScriptRuntime()
	{
		return *m_pScriptRuntime;
	}

	void Scene::PrepareGrassIndirect(RenderView& view)
	{
		const Camera* camera = view.GetCullingCamera();
		if (camera == nullptr)
			return;

		const uint32_t frame = GetFrameIndex();

		/* 相机一档，方向光每档级联各一档。级联不是独立视图，但必须按它自己的
		 * 光源视锥剔：画面外的草照样能把影子甩进来，按相机视锥选会漏掉一批 */
		std::vector<GrassField::IndirectView> views;
		views.reserve(GrassField::kIndirectSlotCount);

		GrassField::IndirectView camera_view;
		const Frustum frustum = Frustum::FromViewProjection(camera->GetViewProjectionMatrix());
		for (int plane = 0; plane < 6; ++plane)
			camera_view.Planes[plane] = NormalizePlane(frustum.Planes[plane]);
		camera_view.View = &view;
		camera_view.Eye = camera->GetPosition();
		camera_view.Slot = 0;
		camera_view.Curve = 0;
		views.push_back(camera_view);

		float shadow_far = 0.0f;
		const SharedPtr<ShadowMapManager>& manager = view.GetShadowMapManager();
		if (view.HasShadowCast() && manager != nullptr)
		{
			const auto& cascades = manager->GetCascadeShadowMaps();
			const glm::vec4& cascade_splits = manager->GetCascadeSplits();
			if (!cascades.empty() && cascades[0]->GetLight() != nullptr)
			{
				const auto& info = cascades[0]->GetLight()->GetShadowMapInfo();
				if (info != nullptr)
					shadow_far = info->ShadowFar;
			}

			for (size_t index = 0; index < cascades.size() && views.size() < GrassField::kIndirectSlotCount;
				++index)
			{
				GrassField::IndirectView shadow_view;
				const Frustum light_frustum = Frustum::FromViewProjection(
					cascades[index]->GetLightViewProjectionMat());
				for (int plane = 0; plane < 6; ++plane)
					shadow_view.Planes[plane] = NormalizePlane(light_frustum.Planes[plane]);
				shadow_view.View = &view;
				shadow_view.Eye = camera->GetPosition();
				shadow_view.Slot = static_cast<uint32_t>(index) + 1u;
				shadow_view.Curve = 1;
				shadow_view.FadeRadius = shadow_far;
				/* 这一档覆盖到相机的哪圈：预算估算按它算投影株数，
				 * 比拿整条阴影距离去估准得多 */
				const float split = index < 4 ? cascade_splits[static_cast<glm::length_t>(index)] : 0.0f;
				shadow_view.Radius = split > 0.0f ? split : shadow_far;
				views.push_back(shadow_view);
			}
		}

		for (auto [entity, grass] : m_Registry.view<GrassComponent>().each())
		{
			if (grass.m_Field == nullptr || grass.m_Field->IsEmpty())
				continue;
			if (!IsEntityInRenderView(entity, view.GetCullingMask()))
				continue;

			for (const GrassField::IndirectView& entry : views)
			{
				if (entry.Slot == 0)
				{
					grass.m_Field->PrepareIndirect(entry, frame);
					continue;
				}

				/* 不投影的草地不占级联槽位，省掉几次全量剔除 */
				if (!grass.m_CastShadow || shadow_far <= 0.0f)
					continue;

				/* 最远距离跟阴影材质同一个口径：组件设了就取较小值。
				 * 这边放宽会让影子拖得比材质允许的远，看起来又长又密 */
				GrassField::IndirectView shadow_entry = entry;
				shadow_entry.FadeRadius = GrassDraw::ShadowFadeRadius(grass, shadow_far);
				if (shadow_entry.FadeRadius <= 0.0f)
					continue;
				grass.m_Field->PrepareIndirect(shadow_entry, frame);
			}
		}
	}

	void Scene::GetPrimaryGrassArea(glm::vec2& area_min, glm::vec2& area_size) const
	{
		area_min = glm::vec2(0.0f);
		area_size = glm::vec2(0.0f);
		for (auto [entity, grass] : m_Registry.view<GrassComponent>().each())
		{
			if (grass.m_Field == nullptr || grass.m_Field->IsEmpty())
				continue;

			/* 脚印图的范围就是这一片草地的世界包围盒 */
			const glm::vec4& rect = grass.m_Field->GetTrampleRect();
			if (rect.z <= 0.0f || rect.w <= 0.0f)
				continue;

			area_min = glm::vec2(rect.x, rect.y);
			area_size = glm::vec2(1.0f / rect.z, 1.0f / rect.w);
			return;
		}
	}

	void Scene::OnUpdate(float delta_time)
	{
		/* 卡顿或断点之后，单帧间隔钳到上限：脚本与动画不该一步跳过整段。
		 * 只影响场景推进，编辑器相机仍拿原始间隔 */
		Simulate(std::min(delta_time, kMaxFrameDeltaSeconds));
	}

	void Scene::Simulate(float delta_time)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

		/* 动画先推进：属性轨道写组件，下面的变换推送才能当帧看到新姿势 */
		m_AnimationRuntime.Tick(*this, delta_time);

		/* 脚本紧随动画：脚本会改变换，必须赶在推送之前跑完 */
		if (m_pScriptRuntime != nullptr)
			m_pScriptRuntime->Tick(*this, delta_time);

		/* 骨骼蒙皮：关节姿势已经定下来，这里把蒙皮模型的顶点重算一遍 */
		SkeletonModel::UpdateScene(*this);

		/* 统一将 TransformComponent 同步给所有 SceneObject（Model / Light / Camera）。
		 * 传下去的是世界变换（沿父链累积），SceneObject 只认识世界空间。
		 * 具体变换如何写入由各 SceneObject 重写的 SetTransform 决定（多态）。 */
		/* 新的一帧换个帧戳：上一帧记下的世界变换全部作废（戳回绕到 0 时整表清掉） */
		m_FrameRecordReadable = false;
		if (++m_FrameWorldStampNow == 0)
		{
			for (FrameRecord& record : m_FrameRecords)
				record.Stamp = 0;
			m_FrameWorldStampNow = 1;
		}

		/* 先把本帧的世界变换全部算出来（局部矩阵按值缓存，没动过的实体只有一次查表），
		 * 渲染侧读到的才是同一份快照。这一步不能省：只有 Transform 的节点也要入快照 */
		{
			PROFILE_CPU_SCOPE_CH(ProfileChannel::Scene, "Scene World Transforms");
			for (const entt::entity entity : m_Registry.view<TransformComponent>())
				FrameWorldTransform(entity);
		}

		/* 再按组件类型分开推送，不再对每个实体挨个 try_get 四种组件：
		 * 模型最多，一个视图走完；光 / 相机 / 探针各自数量很少 */
		PROFILE_CPU_SCOPE_CH(ProfileChannel::Scene, "Push Scene Transforms");
		m_MeshIndex.BeginSync();
		m_SkyBoxEntities.clear();
		for (auto [entity, model_component] : m_Registry.view<ModelComponent>().each())
		{
			if (!model_component.m_Model)
				continue;

			/* 世界变换上面那一趟已经算好，这里直接读记录（只有模型没有 Transform 的畸形实体才现算） */
			const size_t record_index = static_cast<size_t>(entt::to_entity(entity));
			const FrameRecord& record = m_FrameRecords[record_index];
			const glm::mat4& world = (record.Stamp == m_FrameWorldStampNow && record.Entity == entity)
				? record.World : FrameWorldTransform(entity);
			model_component.m_Model->SetTransform(world);

			/* 天空盒铺满全屏、跟网格摆在哪无关：不进空间索引，单独记一份给视图补进列表 */
			bool is_sky = model_component.m_Model->IsSkyBoxMaterial();
			for (const auto& [slot_name, slot_override] : model_component.m_SlotOverrides)
			{
				if (slot_override.pMaterial != nullptr && slot_override.pMaterial->IsSkyBox())
				{
					is_sky = true;
					break;
				}
			}
			if (is_sky)
			{
				if (record.Visible)
					m_SkyBoxEntities.push_back(entity);
				continue;
			}

			/* 空间索引与渲染解耦：这里只写包围盒 / 层 / 显隐，剔除留给查询方。
			 * 蒙皮网格的包围盒是绑定姿态，标成不可剔（进"恒参与"表） */
			const bool skinned = model_component.m_EnableSkinning && !model_component.m_Model->IsStaticModel();
			m_MeshIndex.Sync(entity, world, model_component.m_Model->GetLocalBounds(),
				GetEntityLayerMask(entity), record.Visible, !skinned);
		}
		m_MeshIndex.EndSync();

		/* 地形：高度图只在参数或变换变了才重建（内部有键短路），静止地形不产生每帧开销 */
		for (auto [entity, terrain_component] : m_Registry.view<TerrainComponent>().each())
		{
			if (terrain_component.GetMaterial() == nullptr)
				continue;

			if (terrain_component.m_Field == nullptr)
				terrain_component.m_Field = CreateSharedPtr<TerrainField>();
			terrain_component.m_Field->Rebuild(terrain_component, FrameWorldTransform(entity));
		}

		/* 草地：风相位逐片累加；叶片实例只在参数或变换变了才重建，
		 * GPU 放置时在这里派发计算（还没开始画，命令缓冲借帧头那一份） */
		for (auto [entity, grass_component] : m_Registry.view<GrassComponent>().each())
		{
			/* 相位由速度积分出来，不是直接乘时间：拖动 WindSpeed 只换推进快慢 */
			grass_component.m_PrevWindPhase = grass_component.m_WindPhase;
			grass_component.m_WindPhase += delta_time * grass_component.m_WindSpeed;

			if (grass_component.GetMaterial() == nullptr)
				continue;

			if (grass_component.m_Field == nullptr)
				grass_component.m_Field = CreateSharedPtr<GrassField>();

			/* 贴地：找覆盖这片草地的地形（按草地中心判定），找到就把叶根压到地形表面 */
			const glm::mat4 grass_world = FrameWorldTransform(entity);
			const TerrainField* ground = nullptr;
			if (grass_component.m_ConformToTerrain)
			{
				const glm::vec2 grass_center(grass_world[3].x, grass_world[3].z);
				for (auto [terrain_entity, terrain_component] : m_Registry.view<TerrainComponent>().each())
				{
					(void)terrain_entity;
					if (terrain_component.m_Field != nullptr
						&& terrain_component.m_Field->ContainsPoint(grass_center))
					{
						ground = terrain_component.m_Field.get();
						break;
					}
				}
			}

			/* 开了贴地却没找到地形：给一次提示，否则面板上开关是开的、行为却退回平面，很难查 */
			if (grass_component.m_ConformToTerrain && ground == nullptr && !grass_component.m_WarnedMissingTerrain)
			{
				const char* name = "Grass";
				if (const auto* name_component = m_Registry.try_get<NameComponent>(entity))
					name = name_component->m_Name.c_str();
				CORE_LOG_WARN("Grass '{}' is set to conform to terrain, but no terrain covers its center; "
					"falling back to a flat field.", name);
				grass_component.m_WarnedMissingTerrain = true;
			}

			/* 观察范围：所有相机的并集。只取一个的话，另一个相机（比如编辑器相机）
			 * 看到的地方会没有草 */
			glm::vec2 focus_min(grass_world[3].x, grass_world[3].z);
			glm::vec2 focus_max = focus_min;
			for (RenderView* view : m_RenderViews)
			{
				/* 关掉渲染的视图不参与：它看到的画面不存在，不该撑大生成范围 */
				const Camera* camera = (view != nullptr && view->IsEnableRender())
					? view->GetCullingCamera() : nullptr;
				if (camera == nullptr)
					continue;
				const glm::vec3 position = camera->GetPosition();
				focus_min = glm::min(focus_min, glm::vec2(position.x, position.z));
				focus_max = glm::max(focus_max, glm::vec2(position.x, position.z));
			}

			grass_component.m_Field->Rebuild(grass_component, grass_world, ground, focus_min, focus_max);
			grass_component.m_Field->TickBend(delta_time, grass_component.m_BendRecover);
			grass_component.m_Field->EnsurePlaced();
		}

		/* 压倒草：物体踩着的地方弯下去，走开之后按各自的恢复时间慢慢弹回 */
		for (auto [entity, bend_component] : m_Registry.view<GrassBendComponent>().each())
		{
			if (!bend_component.m_Enabled || !IsEntityVisible(entity))
				continue;

			const glm::vec3 position(FrameWorldTransform(entity)[3]);
			for (auto [grass_entity, grass_component] : m_Registry.view<GrassComponent>().each())
			{
				if (grass_component.m_Field != nullptr)
					grass_component.m_Field->StampBend(position, bend_component.m_Radius);
			}
		}

		for (auto [entity, light_component] : m_Registry.view<LightComponent>().each())
		{
			if (light_component.m_Light)
				light_component.m_Light->SetTransform(FrameWorldTransform(entity));
		}

		for (auto [entity, camera_component] : m_Registry.view<CameraComponent>().each())
		{
			if (camera_component.m_Camera)
				camera_component.m_Camera->SetTransform(FrameWorldTransform(entity));
		}

		for (auto [entity, probe_component] : m_Registry.view<ReflectionProbeComponent>().each())
		{
			if (!probe_component.m_ReflectionProbe)
				continue;

			probe_component.m_ReflectionProbe->SetTransform(FrameWorldTransform(entity));
			/* 可见性同源传播（与光源"隐藏 = 不照"一致）：隐藏的探针不烘焙、
			 * 不参与 IBL 选择（注册保留，恢复可见即恢复参与） */
			probe_component.m_ReflectionProbe->SetEnable(
				m_FrameRecords[static_cast<size_t>(entt::to_entity(entity))].Visible);
		}
		m_FrameRecordReadable = true;

		/* 天空与昼夜：先推进时刻，再把日月星与大气换算成一份状态。
		 * 场景里的方向光按实体顺序绑定（第一个太阳、第二个月亮），方向 / 颜色 / 强度
		 * 全部由这份状态写回 —— 时刻一动，光照跟着走，不用另配一套灯光动画。 */
		Sky::SkyState sky_state;
		{
			SkyComponent* sky_component = nullptr;
			entt::entity sky_owner = entt::null;
			for (auto [entity, component] : m_Registry.view<SkyComponent>().each())
			{
				if (!IsEntityVisible(entity))
					continue;
				if (sky_owner == entt::null || entt::to_integral(entity) < entt::to_integral(sky_owner))
				{
					sky_owner = entity;
					sky_component = &component;
				}
			}

			/* 天空是场景级设置：只配了组件、没摆天空网格时，视图据此补一个内建天空 */
			m_HasActiveSky = sky_component != nullptr;

			if (sky_component != nullptr)
			{
				if (sky_component->m_AutoAdvance && sky_component->m_DayLengthSeconds > 0.0f)
				{
					sky_component->m_Hour += delta_time * 24.0f / sky_component->m_DayLengthSeconds;
					if (sky_component->m_Hour >= 24.0f)
						sky_component->m_Hour = std::fmod(sky_component->m_Hour, 24.0f);
				}

				sky_state = Sky::Resolve(*sky_component);

				/* 取实体 ID 最小的两个方向光：不用额外绑定字段，场景结构本身就是配置 */
				struct DirectionalCandidate
				{
					uint32_t Id{ UINT32_MAX };
					DirectionalLight* Light{ nullptr };
				};
				DirectionalCandidate sun;
				DirectionalCandidate moon;
				for (auto [entity, light_component] : m_Registry.view<LightComponent>().each())
				{
					if (!light_component.m_Light || !IsEntityVisible(entity))
						continue;
					if (light_component.m_Light->GetLightType() != LightType::Directional)
						continue;

					const DirectionalCandidate candidate{ entt::to_integral(entity),
						static_cast<DirectionalLight*>(light_component.m_Light.get()) };
					if (candidate.Id < sun.Id)
					{
						moon = sun;
						sun = candidate;
					}
					else if (candidate.Id < moon.Id)
					{
						moon = candidate;
					}
				}

				if (sun.Light != nullptr)
				{
					sun.Light->SetDirection(-sky_state.SunDirection);
					sun.Light->SetColor(glm::vec4(sky_state.SunColor, 1.0f));
					sun.Light->SetIntensity(sky_state.SunIntensity);
				}
				if (moon.Light != nullptr)
				{
					moon.Light->SetDirection(-sky_state.MoonDirection);
					moon.Light->SetColor(glm::vec4(sky_state.MoonColor, 1.0f));
					moon.Light->SetIntensity(sky_state.MoonIntensity);
				}
			}
		}

		/* 收集RenderView，并在收集前更新 Camera 的视图/投影矩阵 */
		m_RenderViews.clear();
		const auto& camera_entities = m_Registry.view<CameraComponent>();
		for (auto entity : camera_entities)
		{
			auto& camera_component = camera_entities.get<CameraComponent>(entity);
			camera_component.m_Camera->OnUpdate(delta_time);

			/* 相机调试名与实体名同步（GPU 统计树按相机分组展示用），与反射探针同模式 */
			if (const auto* name_component = m_Registry.try_get<NameComponent>(entity);
				name_component && !name_component->m_Name.empty())
				camera_component.m_Camera->SetDebugName(name_component->m_Name);

			auto* render_view = camera_component.m_Camera->GetRenderView();
			render_view->SetOwnerScene(shared_from_this());
			if (render_view->IsEnableRender())
				m_RenderViews.emplace_back(render_view);
		}

		/* 外部相机（编辑器视口 / 工具预览等）：登记后与场景相机一起收集，排在场景相机之后 */
		for (Camera* external_camera : m_ExternalCameras)
		{
			if (external_camera == nullptr)
				continue;

			auto* render_view = external_camera->GetRenderView();
			render_view->SetOwnerScene(shared_from_this());
			if (render_view->IsEnableRender())
				m_RenderViews.emplace_back(render_view);
		}

		/* 按优先级进行排序，优先级大的先画 todo：不需要每帧排序 */
		std::sort(m_RenderViews.begin(), m_RenderViews.end(), [](RenderView* lft, RenderView* rht) {
			if (!lft && !rht) return false;
			if (!lft) return false;
			if (!rht) return true;
			return lft->GetPriority() > rht->GetPriority();
			});

		/* 探针烘焙每帧只推进一次：交给第一个有画面尺寸的视图，不管它是场景相机还是编辑器相机 */
		bool bake_assigned = false;
		for (RenderView* render_view : m_RenderViews)
		{
			const ViewportRegion& region = render_view->GetViewportRegion();
			const bool bake = !bake_assigned && region.Width > 0 && region.Height > 0;
			render_view->SetBakeReflectionProbes(bake);
			bake_assigned = bake_assigned || bake;
		}

		/* 后处理解析（每帧一次）：场景里首个可见且总开关开启的「后处理对象」→ 逐效果采集设置进快照 →
		 * 推给本帧各视图。搜索按实体 ID 升序取首个（确定性）；隐藏 / 总开关关闭 = 不参与（与探针 / 光源同源）。
		 * 无对象 = 空快照 = 全部效果缺省关闭（旧场景零变化）。 */
		PostProcess::PostProcessState post_process_state;
		{
			entt::entity owner_id = entt::null;
			for (const entt::entity entity_id : m_Registry.view<PostProcessComponent>())
			{
				if (!IsEntityVisible(entity_id) || !m_Registry.get<PostProcessComponent>(entity_id).m_Enabled)
					continue;
				if (owner_id == entt::null || entt::to_integral(entity_id) < entt::to_integral(owner_id))
					owner_id = entity_id;
			}

			if (owner_id != entt::null)
			{
				const Entity owner{ owner_id, shared_from_this() };
				for (const PostProcess::EffectDesc& effect : PostProcess::PostProcessEffectRegistry::Instance().All())
				{
					if (effect.Capture != nullptr)
						effect.Capture(owner, post_process_state);
				}
			}
		}

		for (RenderView* render_view : m_RenderViews)
		{
			if (render_view != nullptr)
			{
				render_view->SetPostProcessState(post_process_state);
				render_view->SetSkyState(sky_state);
			}
		}

		/* 脚本请求的销毁放到最后：脚本 tick 与各趟视图遍历都跑完，registry 稳定了再删 */
		FlushPendingDestroy();
	}

	/* 执行本帧收集到的所有渲染视图。
	 * 帧级准备（GPU 计时器等）由帧循环统一做一次，见 Application::Run。 */
	void Scene::Render()
	{
		for (const auto& view : m_RenderViews)
		{
			Renderer::RenderAView(view);
		}

		/* 渲染完之后的改动（面板 / Gizmo）要现算，等下一次 OnUpdate 重新记 */
		m_FrameRecordReadable = false;
	}

	Entity Scene::GetPrimaryCameraEntity()
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

		/* difference of view and group: https://github.com/skypjack/entt/discussions/638 */
		const auto entity_view = m_Registry.view<CameraComponent>();
		for (auto& entity : entity_view)
		{
			const auto& camera_component = entity_view.get<CameraComponent>(entity);
			if (camera_component.m_IsPrimary)
				return Entity{ entity, shared_from_this() };
		}
		return {};
	}

	const std::vector<std::string>& Scene::GetComponentOrder(entt::entity entity) const
	{
		static const std::vector<std::string> empty;
		const auto* order = m_Registry.try_get<ComponentOrderComponent>(entity);
		return (order != nullptr) ? order->m_Components : empty;
	}

	void Scene::SetComponentOrder(entt::entity entity, std::vector<std::string> components)
	{
		if (!m_Registry.valid(entity))
			return;

		/* 空表就是回到注册顺序，不挂一个空组件 */
		if (components.empty())
		{
			m_Registry.remove<ComponentOrderComponent>(entity);
			return;
		}

		m_Registry.emplace_or_replace<ComponentOrderComponent>(entity, std::move(components));
	}

	void Scene::Serializer(const std::string& path, bool write_bake_cache)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

		std::filesystem::path scene_path;
		if (!TryPathFromUtf8(path, scene_path))
		{
			CORE_LOG_ERROR("Invalid UTF-8 scene path.");
			return;
		}
		auto scene_file = std::unique_ptr<FILE, decltype(&std::fclose)>(
			OpenUtf8File(scene_path, "wb"), &std::fclose);
		if (!scene_file)
		{
			CORE_LOG_ERROR("Failed to open scene file for writing: {}.", path);
			return;
		}

		tinyxml2::XMLDocument doc;
		SerializeDocument(doc, write_bake_cache ? &path : nullptr);

		if (doc.SaveFile(scene_file.get()) != tinyxml2::XML_SUCCESS)
			CORE_LOG_ERROR("Failed to write scene file: {}.", path);
	}

	/* 存的是作者数据：先停播并还原姿势，别把当前帧的采样值写进场景（克隆也走这里） */
	void Scene::SerializeDocument(tinyxml2::XMLDocument& doc, const std::string* bake_cache_path)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

		m_AnimationRuntime.StopAll(*this);

		doc.InsertEndChild(doc.NewDeclaration());
		tinyxml2::XMLElement* scene_root = doc.NewElement("Scene");
		doc.InsertEndChild(scene_root);

		tinyxml2::XMLElement* entities_root = scene_root->InsertNewChildElement("Entities");

		/* 保存场景时才持久化烘焙结果：这里只登记请求（真正的写入等 GPU 执行完、由探针状态机在后续
		 * 帧完成），但缓存路径要在序列化组件之前定下来（一并写进 .scn）；缓存按正在保存的场景文件
		 * 位置分目录。 */
		if (m_pReflectionProbeManager != nullptr && bake_cache_path != nullptr)
			m_pReflectionProbeManager->RequestBakeCacheWrites(*bake_cache_path);

		/* 按实体句柄升序输出：registry 的遍历顺序与创建顺序无关，
		 * 直接遍历会让每次「加载 → 保存」把实体顺序整体翻转。 */
		std::vector<entt::entity> entities;
		m_Registry.each([&entities](auto entity_id) { entities.emplace_back(entity_id); });
		std::sort(entities.begin(), entities.end(),
			[](entt::entity lhs, entt::entity rhs)
			{
				return entt::to_integral(lhs) < entt::to_integral(rhs);
			});

		for (const auto entity_id : entities)
		{
			Entity entity = { entity_id, shared_from_this() };
			if (!entity)
				continue;

			SerializeEntity(entities_root, entity);
		}
	}

	bool Scene::Deserializer(const std::string& path)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

		std::filesystem::path scene_path;
		if (!TryPathFromUtf8(path, scene_path))
		{
			CORE_LOG_ERROR("Invalid UTF-8 scene path.");
			return false;
		}
		auto scene_file = std::unique_ptr<FILE, decltype(&std::fclose)>(
			OpenUtf8File(scene_path, "rb"), &std::fclose);
		if (!scene_file)
		{
			CORE_LOG_ERROR("Failed to open scene file: {}.", path);
			return false;
		}

		tinyxml2::XMLDocument doc;
		if (doc.LoadFile(scene_file.get()) != tinyxml2::XML_SUCCESS)
		{
			CORE_LOG_ERROR("Failed to deserializer scene file: {}.", path);
			return false;
		}

		return DeserializeDocument(doc, false);
	}

	SharedPtr<Scene> Scene::Clone()
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

		tinyxml2::XMLDocument doc;
		SerializeDocument(doc, nullptr);

		SharedPtr<Scene> clone = CreateSharedPtr<Scene>();
		if (!clone->DeserializeDocument(doc, true))
			return nullptr;
		return clone;
	}

	bool Scene::DeserializeDocument(const tinyxml2::XMLDocument& doc, bool preserve_handles)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

		const tinyxml2::XMLElement* scene_root = doc.FirstChildElement("Scene");
		const tinyxml2::XMLElement* entities_root = (scene_root != nullptr)
			? scene_root->FirstChildElement("Entities") : nullptr;
		if (entities_root == nullptr)
		{
			CORE_LOG_ERROR("Scene data has no <Scene>/<Entities>.");
			return false;
		}

		/* 加载是「替换」语义：不清空会把上一份场景的实体留在新场景里。
		 * 先确认文件可加载再清空，加载失败时当前内容不受影响。 */
		ClearAllEntities();

		/* 父子关系存的是父实体的句柄，加载后句柄会重新分配，
		 * 所以先建完所有实体并记下「文件里的 ID → 新实体」，再统一还原层级。 */
		std::vector<std::pair<const tinyxml2::XMLElement*, Entity>> loaded_entities;
		std::unordered_map<uint64_t, Entity> entities_by_file_id;

		for (const tinyxml2::XMLElement* entity_root = entities_root->FirstChildElement(); entity_root; entity_root = entity_root->NextSiblingElement("Entity"))
		{
			const tinyxml2::XMLAttribute* id_attribute = entity_root->FindAttribute("ID");
			if (id_attribute == nullptr)
				continue;

			const char* name = entity_root->Attribute("Name");
			if (name == nullptr || *name == '\0')
				continue;

			/* 克隆要保住句柄，跨世界的选中 / 地形高度都按它对上；打开文件仍走紧凑分配 */
			const entt::entity preserved = preserve_handles
				? static_cast<entt::entity>(static_cast<uint32_t>(id_attribute->Unsigned64Value()))
				: entt::null;
			Entity entity = CreateEntityInternal(name, preserved);

			/* Name / Transform 由 CreateEntity 带上，其余组件由注册表驱动 */
			for (const ComponentDesc& desc : ComponentRegistry::Instance().All())
			{
				if (!desc.bSerializable || desc.Has == nullptr || desc.GetPtr == nullptr || desc.Add == nullptr)
					continue;

				const tinyxml2::XMLElement* element = entity_root->FirstChildElement(desc.Name);
				if (element == nullptr)
					continue;

				if (!desc.Has(entity))
					desc.Add(entity);

				void* component = desc.GetPtr(entity);
				if (component != nullptr)
					LoadComponentFromXml(desc, component, element);
			}

			/* 属性面板卡片顺序（旧文件没有这个属性）：逗号分隔的组件名 */
			if (const char* card_order = entity_root->Attribute("CardOrder"); card_order != nullptr)
			{
				std::vector<std::string> components = SplitString(card_order, ',');
				if (!components.empty())
					m_Registry.emplace<ComponentOrderComponent>(entity, std::move(components));
			}

			entities_by_file_id.emplace(id_attribute->Unsigned64Value(), entity);
			loaded_entities.emplace_back(entity_root, entity);
		}

		/* 第二遍：还原父子关系。局部变换已经在第一遍按原样读入，
		 * 所以这里只补链接，不重算变换（否则世界位置会变）。 */
		for (const auto& [entity_root, entity] : loaded_entities)
		{
			const tinyxml2::XMLAttribute* parent_attribute = entity_root->FindAttribute("Parent");
			if (parent_attribute == nullptr)
				continue;

			const auto parent = entities_by_file_id.find(parent_attribute->Unsigned64Value());
			if (parent == entities_by_file_id.end())
				continue; /* 指向不存在的实体：按根节点处理 */

			/* 文件被改坏到出现环时直接断开，避免层级遍历无限上溯 */
			if (IsAncestor(static_cast<entt::entity>(entity), static_cast<entt::entity>(parent->second)))
				continue;

			SetParentLink(entity, static_cast<entt::entity>(parent->second));
		}

		return true;
	}

	void Scene::SerializeEntity(tinyxml2::XMLElement* root_node, Entity& entity)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Scene);

		tinyxml2::XMLElement* entity_root = root_node->InsertNewChildElement("Entity");

		/* ID */
		entity_root->SetAttribute("ID", (uint32_t)entity);

		/* Name 由实体自身的属性承载 */
		if (entity.HasComponent<NameComponent>())
			entity_root->SetAttribute("Name", entity.GetComponent<NameComponent>().m_Name.c_str());

		/* 父节点写在实体自己的属性上（同 ID / Name）：它是场景图结构，不是某个组件的字段。
		 * 存的是父实体的句柄，加载时按 ID 重新映射（见 Deserializer）。 */
		if (const entt::entity parent = GetParent(entity); parent != entt::null)
			entity_root->SetAttribute("Parent", static_cast<uint32_t>(parent));

		/* 属性面板卡片顺序：名字用逗号连成一个属性，省得多一层元素 */
		if (const auto* card_order = m_Registry.try_get<ComponentOrderComponent>(entity);
			card_order != nullptr && !card_order->m_Components.empty())
		{
			entity_root->SetAttribute("CardOrder", JoinStrings(card_order->m_Components, ',').c_str());
		}

		/* 其余组件由注册表驱动，元素名即组件名 */
		for (const ComponentDesc& desc : ComponentRegistry::Instance().All())
		{
			if (!desc.bSerializable || desc.Has == nullptr || desc.GetPtr == nullptr)
				continue;

			if (!desc.Has(entity))
				continue;

			void* component = desc.GetPtr(entity);
			if (component == nullptr)
				continue;

			SaveComponentToXml(desc, component, entity_root->InsertNewChildElement(desc.Name));
		}
	}

    /* 对各组件类型连接on_construct/on_destroy信号 */
    template<typename... T>
    void Scene::ConnectSignalsForComponents(std::tuple<T...>)
    {
        (
            (m_Registry.on_construct<T>().template connect<&Scene::OnConstructComponent<T>>(this), ...),
            (m_Registry.on_destroy<T>().template connect<&Scene::OnDestroyComponent<T>>(this), ...)
            );
    }

    /* 对各组件类型断开on_construct/on_destroy信号（用于Scene析构阶段） */
    template<typename... T>
    void Scene::DisconnectSignalsForComponents(std::tuple<T...>)
    {
        (
            (m_Registry.on_construct<T>().disconnect(this), ...),
            (m_Registry.on_destroy<T>().disconnect(this), ...)
            );
    }

	/* on_construct关联组件的OnAdded */
	template<typename T>
	void Scene::OnConstructComponent(entt::registry&, entt::entity entity)
	{
		auto& component = m_Registry.get<T>(entity);
		if constexpr (std::is_base_of_v<ComponentBase, T>)
		{
			Entity e{ entity, shared_from_this() };
			component.OnAdded(*this, e);
		}
	}

	/* on_destroy关联组件的OnRemoved */
	template<typename T>
	void Scene::OnDestroyComponent(entt::registry&, entt::entity entity)
	{
		auto& component = m_Registry.get<T>(entity);
		if constexpr (std::is_base_of_v<ComponentBase, T>)
		{
			Entity e{ entity, shared_from_this() };
			component.OnRemoved(*this, e);
		}
	}
}
