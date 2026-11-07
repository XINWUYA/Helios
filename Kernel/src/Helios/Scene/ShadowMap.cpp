#include "Pch.h"
#include "ShadowMap.h"
#include "Scene.h"
#include <cmath>
#include <cstring>
#include <glm/gtc/type_ptr.hpp>
#include <Helios/Common/Math.h>
#include <Helios/Profiler/RenderStatistics.h>
#include <Helios/VirtualDevice/DeviceTexture.h>
#include <Helios/Renderer/FrameGraph/FrameGraph.h>
#include <Helios/Renderer/RenderView.h>
#include <Helios/Profiler/Profiler.h>
#include <Helios/Renderer/Renderer.h>
#include <Helios/Renderer/RenderPasses/ObjectSorting.h>
#include <Helios/Renderer/RenderPasses/GrassDrawParams.h>
#include <Helios/Renderer/RenderPasses/TerrainPasses.h>
#include <Helios/Scene/TerrainField.h>
#include <Helios/Scene/Material.h>
#include <Helios/Core/AssetManager.h>
#include <Helios/VirtualDevice/DeviceFrameBuffer.h>
#include <Helios/Scene/Camera.h>
#include "Mesh.h"
#include "SceneCommon.h"
#include "Components.h"
#include "Model.h"

namespace Helios
{
	namespace
	{
		struct GroundBounds
		{
			glm::vec3 Min{ 0.0f };
			glm::vec3 Max{ 0.0f };
		};

		uint64_t MixSignature(uint64_t seed, uint64_t value)
		{
			return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
		}

		uint64_t MixSignature(uint64_t seed, const glm::vec3& value)
		{
			for (int axis = 0; axis < 3; ++axis)
			{
				uint32_t bits = 0;
				std::memcpy(&bits, &value[axis], sizeof(bits));
				seed = MixSignature(seed, bits);
			}
			return seed;
		}

		/* 地形和草地不在可见网格表里，级联分割区间得靠它们的包围盒撑开，
		 * 否则场景里只剩一个小模型时级联全挤在它周围，远处的草和地面没有影子。
		 * 返回签名，高度改过或包围盒变了都会变 */
		uint64_t CollectGroundBounds(Scene& scene, LayerMask culling_mask, std::vector<GroundBounds>& out)
		{
			uint64_t signature = 0;
			for (auto [entity, terrain] : scene.GetRegistry().view<TerrainComponent>().each())
			{
				if (terrain.m_Field == nullptr || terrain.m_Field->IsEmpty()
					|| !scene.IsEntityInRenderView(entity, culling_mask))
					continue;

				const glm::vec3 origin = terrain.m_Field->GetOrigin();
				const glm::vec2 half = terrain.m_Field->GetWorldHalfExtent();
				GroundBounds bounds;
				bounds.Min = glm::vec3(origin.x - half.x, terrain.m_Field->GetMinWorldHeight(), origin.z - half.y);
				bounds.Max = glm::vec3(origin.x + half.x, terrain.m_Field->GetMaxWorldHeight(), origin.z + half.y);
				out.push_back(bounds);
				signature = MixSignature(signature, terrain.m_Field->GetRevision());
			}

			for (auto [entity, grass] : scene.GetRegistry().view<GrassComponent>().each())
			{
				if (grass.m_Field == nullptr || grass.m_Field->IsEmpty()
					|| !scene.IsEntityInRenderView(entity, culling_mask))
					continue;

				GroundBounds bounds{ glm::vec3(std::numeric_limits<float>::max()),
					glm::vec3(-std::numeric_limits<float>::max()) };
				for (const GrassField::Chunk& chunk : grass.m_Field->GetChunks())
				{
					if (chunk.Count == 0)
						continue;
					bounds.Min = glm::min(bounds.Min, chunk.Center - chunk.Extent);
					bounds.Max = glm::max(bounds.Max, chunk.Center + chunk.Extent);
				}
				if (bounds.Min.x <= bounds.Max.x)
					out.push_back(bounds);
			}

			for (const GroundBounds& bounds : out)
				signature = MixSignature(MixSignature(signature, bounds.Min), bounds.Max);
			return signature;
		}
	}

	namespace
	{
		constexpr uint32_t kFallbackCascadeCount = 4;

		/* 点光立方体各面视锥的半角（90° 全角） */
		constexpr float kPointLightFaceFov = 90.0f;

		/* 级联剔除的余量（占投射物自身包围盒尺寸的比例）：AABB 按绑定姿态算，
		 * 蒙皮动画会把顶点带到盒子外，留点余量免得影子在级联边界闪 */
		constexpr float kCasterCullMargin = 0.1f;

		/* 阴影图数组的 FrameGraph 资源名：纹理创建与抓取逐层注册共用（避免两处各自漂移） */
		constexpr const char* kShadowMapTextureName = "ShadowMap";
	}

	ShadowMap::ShadowMap(const SharedPtr<Light>& light, uint16_t shadow_idx, uint8_t face_idx)
		: m_pLight(light), m_ShadowIndex(shadow_idx), m_FaceIndex(face_idx)
	{
	}

	glm::mat4 ShadowMap::GetDirectionalLightViewMatrix(const glm::vec3& direction, const glm::vec3& origin) noexcept
	{
		auto up = glm::vec3(0.0f, 1.0f, 0.0f);
		auto front = direction;
		if (std::abs(glm::dot(front, up)) > 0.999f)
		{ // looking straight up
			up = { up.z, up.x, up.y };
		}

		auto right = glm::normalize(glm::cross(front, up));
		up = glm::cross(right, front);

		const glm::mat4 transform = {
			glm::vec4(right, 0.0f),
			glm::vec4(up, 0.0f),
			glm::vec4(-front, 0.0f),
			glm::vec4(origin, 1.0f)
		};

		return glm::inverse(transform);
	}

	glm::mat4 ShadowMap::GetPunctualLightViewMatrix(uint8_t face_idx, const glm::vec3& origin) noexcept
	{
		glm::vec3 direction;
		if (face_idx == 0) direction = { 1.0f, 0.0f, 0.0f }; // PositiveX
		else if (face_idx == 1) direction = { -1.0f, 0.0f, 0.0f }; // NegativeX
		else if (face_idx == 2) direction = { 0.0f, 1.0f, 0.0f }; // PositionY
		else if (face_idx == 3) direction = { 0.0f, -1.0f, 0.0f }; // NegativeY
		else if (face_idx == 4) direction = { 0.0f, 0.0f, 1.0f }; // PositiveZ
		else if (face_idx == 5) direction = { 0.0f, 0.0f, -1.0f }; // -PositionZ

		return GetDirectionalLightViewMatrix(direction, origin);
	}

	void ShadowMapManager::RegisterShadowLight(const SharedPtr<Light>& light)
	{
		if (!light || !light->IsCastShadow())
			return;

		/* 阴影配置由 SetIsCastShadow(true) 备好（含场景加载与面板勾选两条路径）；
		 * 仍为空时不注册，避免解引用空配置。 */
		if (auto& shadow_map_info = light->GetShadowMapInfo())
		{
			switch (light->GetLightType())
			{
			case LightType::Directional:
				ASSERT(shadow_map_info->CascadeCnt <= SHADOW_CASCADE_MAX_NUM);
				for (uint8_t cascade_id = 0; cascade_id < shadow_map_info->CascadeCnt; ++cascade_id)
				{
					auto shadow_map = CreateSharedPtr<ShadowMap>(light, cascade_id, 0);
					m_CascadeShadowMaps.push_back(shadow_map);
				}
				break;
			case LightType::Point:
			{
				/* 点光 = 立方体 6 面，每面一个固定朝向的 90° 投影
				 * （面顺序与 GetPunctualLightViewMatrix 一致：+X,-X,+Y,-Y,+Z,-Z）；
				 * 超出层预算时整个光源放弃注册，保证同一光源的面在数组里连续。 */
				if (m_PunctualShadowMaps.size() + 6 > SHADOW_PUNCTUAL_MAX_NUM)
					break;
				for (uint8_t face = 0; face < 6; ++face)
				{
					auto shadow_map = CreateSharedPtr<ShadowMap>(light, 0, face);
					m_PunctualShadowMaps.push_back(shadow_map);
				}
				break;
			}
			case LightType::Spot:
				/* 聚光 = 单面，投影视锥即为光锥（轴心 = 光源位置，朝向 = 光向） */
				if (m_PunctualShadowMaps.size() + 1 > SHADOW_PUNCTUAL_MAX_NUM)
					break;
				m_PunctualShadowMaps.push_back(CreateSharedPtr<ShadowMap>(light, 0, 0));
				break;
			default:
				break;
			}

			/* 阴影纹理大小 */
			m_MaxDimension = std::max(m_MaxDimension, shadow_map_info->Size);
		}
	}

	/* 准备ShadowMapArray, 先存放方向光的Shadow */
	void ShadowMapManager::PrepareForShadowMaps(const SharedPtr<Scene>& scene, const Camera* camera)
	{
		if (!m_ShadowCasterShader)
			m_ShadowCasterShader = AssetManager::Instance().GetOrLoad<DeviceShader>(ABSOLUTE_PATH("Shaders/Builtin/ShadowCaster.glsl"));
		if (!m_GrassShadowShader)
			m_GrassShadowShader = AssetManager::Instance().GetOrLoad<DeviceShader>(ABSOLUTE_PATH("Shaders/Grass/GrassShadow.glsl"));

		PrepareRequiredTexture();

		/* 准备方向光 ShadowMap：算级联 VP 矩阵和分割距离。这里拿不到可见网格列表，先按相机
		 * 子视锥拟合一次；RenderView::Execute 随后会带着投射物重新拟合（脏检测把投射物输入
		 * 也算上），保证投射物不被 near / far 板裁掉。 */
		UpdateCascadeMatrices(camera, nullptr, scene.get());
	}

	/* 依据当前方向光方向、相机与场景投射物，重算各级联的视图投影矩阵与分割距离。
	 * 供 PrepareForShadowMaps（首次准备）与每帧执行前（方向光旋转 / 相机移动）共用。
	 * 通过对比缓存（方向光方向 + 相机视图/投影矩阵 + 是否拿到投射物）跳过无变化的重算。 */
	void ShadowMapManager::UpdateCascadeMatrices(const Camera* camera, const std::vector<VisibleMeshObject>* mesh_objects,
		Scene* scene, LayerMask culling_mask)
	{
		/* 准备方向光的ShadowMap */
		if (m_CascadeShadowMaps.empty())
			return;

		auto& shadow_map = m_CascadeShadowMaps[0];
		auto light = std::dynamic_pointer_cast<DirectionalLight>(shadow_map->m_pLight);
		auto& direction = light->GetDirection();
		auto& shadow_map_info = light->GetShadowMapInfo();

		/* 脏检测：方向光方向与相机（视图/投影矩阵）均未变化且非强制脏时，跳过重算。
		 * 使用较小 epsilon 容忍浮点误差，避免静止时因精度抖动误触发重算。
		 * 注：GLM 的 glm::equal 不支持 mat4，故矩阵需逐元素比较。 */
		const glm::mat4 view_mat = camera ? camera->GetViewMatrix() : glm::mat4(1.0f);
		const glm::mat4 proj_mat = camera ? camera->GetProjectionMatrix() : glm::mat4(1.0f);
		constexpr float eps = 1e-5f;
		const auto mat_equal = [eps](const glm::mat4& a, const glm::mat4& b)
		{
			const float* pa = glm::value_ptr(a);
			const float* pb = glm::value_ptr(b);
			for (int i = 0; i < 16; ++i)
				if (std::fabs(pa[i] - pb[i]) > eps)
					return false;
			return true;
		};
		const bool dir_equal =
			std::fabs(direction.x - m_LastLightDir.x) <= eps &&
			std::fabs(direction.y - m_LastLightDir.y) <= eps &&
			std::fabs(direction.z - m_LastLightDir.z) <= eps;
		/* 可见网格数量：级联剔除表按它生成，数量变了表就过期 */
		const uint32_t caster_count = (mesh_objects != nullptr)
			? static_cast<uint32_t>(mesh_objects->size()) : 0u;

		/* 地形与草地重建或挪动过也要重算：分割区间和深度板都按它们拟合 */
		std::vector<GroundBounds> ground_bounds;
		const uint64_t ground_signature = (scene != nullptr)
			? CollectGroundBounds(*scene, culling_mask, ground_bounds) : 0u;

		const bool unchanged =
			!m_IsDirty &&
			dir_equal &&
			mat_equal(view_mat, m_LastViewMat) &&
			mat_equal(proj_mat, m_LastProjMat) &&
			caster_count == m_LastCasterCount &&
			ground_signature == m_LastGroundSignature;
		if (unchanged)
			return;

		/* 记录本次用于计算的输入，供下次对比 */
		m_LastLightDir = direction;
		m_LastViewMat = view_mat;
		m_LastProjMat = proj_mat;
		m_LastCasterCount = caster_count;
		m_LastGroundSignature = ground_signature;
		m_IsDirty = false;


		const uint8_t cascade_cnt = shadow_map_info->CascadeCnt;
		ASSERT(cascade_cnt <= SHADOW_CASCADE_MAX_NUM);

		// 计算光照视图矩阵（所有级联共享）
		auto light_view_mat = ShadowMap::GetDirectionalLightViewMatrix(direction);

		/* CSM 子视锥分割：按相机视距把主视锥切成 cascade_cnt 段，每段对应一个级联。用对数分割与
		 * 均匀分割的混合（λ 混合），近处精度和远处稳定性兼顾。split[i] = 第 i 段远边界在视图空间的距离。 */
		const float lambda = 0.3f;// 0.5f; // 0 = 均匀，1 = 纯对数
		const float cam_near = camera ? camera->GetNearClip() : 0.1f;
		/* 阴影只需覆盖到 ShadowFar（>0 时生效）：超出该距离的区域不再产生阴影，
		 * 避免把级联预算浪费在相机 far 之外的部分 */
		float cam_far = camera ? camera->GetFarClip() : shadow_map_info->ShadowFar;
		if (shadow_map_info->ShadowFar > 0.0f)
			cam_far = std::min(cam_far, shadow_map_info->ShadowFar);
		if (cam_far <= cam_near)
			cam_far = cam_near + 1.0f;

		/* 收集场景几何体在光照空间的 AABB（每个 MeshSegment 一个）。级联的深度板不能只看相机
		 * 子视锥：空气和近相机那一段会把 z 跨度撑大，深度图偏黑、精度变差；所以要按"与该级联 xy
		 * 相交的几何体"来拟合 near / far。顺带记下几何在视线方向的深度区间，分割按它取界。 */
		struct LightSpaceBounds { glm::vec3 Min; glm::vec3 Max; uint32_t ObjectIndex{ 0 }; };
		std::vector<LightSpaceBounds> caster_bounds;
		float scene_near = std::numeric_limits<float>::max();
		float scene_far = -std::numeric_limits<float>::max();
		if (mesh_objects)
		{
			caster_bounds.reserve(mesh_objects->size());
			for (uint32_t object_index = 0; object_index < static_cast<uint32_t>(mesh_objects->size()); ++object_index)
			{
				const auto& mesh_object = (*mesh_objects)[object_index];
				if (!mesh_object.MeshSegment)
					continue;

				/* 天空盒不参与拟合：它铺满全屏（顶点着色器置 z = 远平面），
				 * 包围盒会把光源视锥的深度范围撑飞 */
				if (mesh_object.Material && mesh_object.Material->IsSkyBox())
					continue;

				const glm::vec3& aabb_min = mesh_object.MeshSegment->GetAABBMin();
				const glm::vec3& aabb_max = mesh_object.MeshSegment->GetAABBMax();
				LightSpaceBounds bounds{
					glm::vec3(std::numeric_limits<float>::max()),
					glm::vec3(-std::numeric_limits<float>::max()) };
				for (int corner = 0; corner < 8; ++corner)
				{
					const glm::vec3 local_corner(
						(corner & 1) ? aabb_max.x : aabb_min.x,
						(corner & 2) ? aabb_max.y : aabb_min.y,
						(corner & 4) ? aabb_max.z : aabb_min.z);
					const glm::vec3 world_corner = glm::vec3(mesh_object.Local2WorldMat * glm::vec4(local_corner, 1.0f));
					const glm::vec3 ls_corner = glm::vec3(light_view_mat * glm::vec4(world_corner, 1.0f));
					bounds.Min = glm::min(bounds.Min, ls_corner);
					bounds.Max = glm::max(bounds.Max, ls_corner);

					/* 深度是线性函数，极值必在角点上 */
					const float depth = -(view_mat * glm::vec4(world_corner, 1.0f)).z;
					scene_near = std::min(scene_near, depth);
					scene_far = std::max(scene_far, depth);
				}
				caster_bounds.emplace_back(LightSpaceBounds{ bounds.Min, bounds.Max, object_index });
			}
		}

		for (const GroundBounds& bounds : ground_bounds)
		{
			for (int corner = 0; corner < 8; ++corner)
			{
				const glm::vec3 world_corner(
					(corner & 1) ? bounds.Max.x : bounds.Min.x,
					(corner & 2) ? bounds.Max.y : bounds.Min.y,
					(corner & 4) ? bounds.Max.z : bounds.Min.z);
				const float depth = -(view_mat * glm::vec4(world_corner, 1.0f)).z;
				scene_near = std::min(scene_near, depth);
				scene_far = std::max(scene_far, depth);
			}
		}

		/* 分割区间贴着可见几何的实际深度：相机离场景很远时起点后移到最近几何，级联不会整段
		 * 落在空气里；相机 far 或 ShadowFar 比场景大很多时终点也收回来，免得盒子摊到上千单位、
		 * 物体在阴影图里只剩几个像素。几何全在阴影距离外或没有几何时，退回相机区间。 */
		float range_near = cam_near;
		float range_far = cam_far;
		if (scene_near <= scene_far && scene_near < cam_far)
		{
			range_near = std::min(std::max(scene_near, cam_near), cam_far);
			range_far = std::min(std::max(scene_far, range_near), cam_far);
		}

		std::vector<float> split_dist(cascade_cnt + 1);
		range_near = std::max(range_near, 1e-3f); // 对数分割要除以起点
		split_dist[0] = range_near;
		for (uint8_t i = 1; i <= cascade_cnt; ++i)
		{
			const float f = static_cast<float>(i) / static_cast<float>(cascade_cnt);
			const float log_split = range_near * std::pow(range_far / range_near, f);
			const float uni_split = range_near + (range_far - range_near) * f;
			split_dist[i] = lambda * log_split + (1.0f - lambda) * uni_split;
		}

		// 相机世界矩阵（用于把视图空间子视锥角点变换回世界空间）；view_mat 已在脏检测处获取
		const glm::mat4 cam_world_mat = glm::inverse(view_mat);

		/* 覆盖范围只由子视锥自己定：不拿全场景并集去裁 XY。并集 AABB 会把远级联的盒子
		 * 掐成一条偏离视锥的窄条，接收面直接落到盒子外（阴影缺失 / 错位）。 */

		// 透视相机视锥参数（用于构造子视锥角点）；非透视（正交）相机退化为整段
		const float fov        = camera ? camera->GetFov() : 45.0f;
		const float aspect     = camera ? camera->GetAspectRatio() : 1.0f;
		const float tan_half_fov = std::tan(glm::radians(fov) * 0.5f);

		// 各级联在视图空间的分割距离（远边界），写回 UBO 供着色阶段选级联
		glm::vec4 cascade_splits(0.0f);

		// 每级联：子视锥角点 → 光照空间，包围球定范围、角点中点定中心
		for (uint8_t cascade_id = 0; cascade_id < cascade_cnt; ++cascade_id)
		{
			const float seg_near = split_dist[cascade_id];
			const float seg_far  = split_dist[cascade_id + 1];

			/* 近端往回让出上一段的过渡带：采样端会在上一段 split 前 band 宽的范围里混采本级联，
			 * 这段接收面必须落在本级的正交盒内（xy 与深度都要）。 */
			const float prev_near = (cascade_id > 0) ? split_dist[cascade_id - 1] : range_near;
			const float blend_band = (seg_near - prev_near) * kCascadeBlendRatio;
			const float fit_near = std::max(seg_near - blend_band, range_near);

			// 子视锥在视图空间（相机看向 -Z）的 8 个角点
			const float half_h_near = fit_near * tan_half_fov;
			const float half_w_near = half_h_near * aspect;
			const float half_h_far  = seg_far  * tan_half_fov;
			const float half_w_far  = half_h_far * aspect;

			glm::vec3 corners[8];
			// 近平面 4 角（z = -fit_near）
			corners[0] = glm::vec3(-half_w_near, -half_h_near, -fit_near);
			corners[1] = glm::vec3( half_w_near, -half_h_near, -fit_near);
			corners[2] = glm::vec3( half_w_near,  half_h_near, -fit_near);
			corners[3] = glm::vec3(-half_w_near,  half_h_near, -fit_near);
			// 远平面 4 角（z = -seg_far）
			corners[4] = glm::vec3(-half_w_far, -half_h_far, -seg_far);
			corners[5] = glm::vec3( half_w_far, -half_h_far, -seg_far);
			corners[6] = glm::vec3( half_w_far,  half_h_far, -seg_far);
			corners[7] = glm::vec3(-half_w_far,  half_h_far, -seg_far);

			// 变换到世界空间，再变换到光照空间，取 AABB
			glm::vec3 ls_min(std::numeric_limits<float>::max());
			glm::vec3 ls_max(-std::numeric_limits<float>::max());
			for (int i = 0; i < 8; ++i)
			{
				const glm::vec3 world_corner = glm::vec3(cam_world_mat * glm::vec4(corners[i], 1.0f));
				const glm::vec3 ls_corner = glm::vec3(light_view_mat * glm::vec4(world_corner, 1.0f));
				ls_min = glm::min(ls_min, ls_corner);
				ls_max = glm::max(ls_max, ls_corner);
			}

			/* 范围用包围球而不是紧 AABB：半径只由该级联的距离区间与 fov 决定，与相机朝向、
			 * 场景内容无关 —— 相机转动时盒子尺寸不变，配合纹素吸附不会抖。球心取角点 AABB
			 * 的中点（角点关于子视锥轴心对称，两者等价），盒子里必然装得下整个子视锥。 */
			const float tan_diag = tan_half_fov * std::sqrt(1.0f + aspect * aspect);
			const float far_extent = seg_far * tan_diag;
			const float half_depth = (seg_far - fit_near) * 0.5f;
			float radius = std::sqrt(far_extent * far_extent + half_depth * half_depth);
			// 下限：只放大、不缩小（CascadeRadius 是"最小半边长"）
			if (const float min_radius = shadow_map_info->CascadeRadius[cascade_id]; min_radius > 0.0f)
				radius = std::max(radius, min_radius);

			/* 纹素吸附：边长固定，吸附网格也就固定，相机平移时阴影不抖 */
			const float texel_size = (2.0f * radius) / static_cast<float>(shadow_map_info->Size);
			const float center_x = std::round((ls_min.x + ls_max.x) * 0.5f / texel_size) * texel_size;
			const float center_y = std::round((ls_min.y + ls_max.y) * 0.5f / texel_size) * texel_size;

			const float ortho_left   = center_x - radius;
			const float ortho_right  = center_x + radius;
			const float ortho_bottom = center_y - radius;
			const float ortho_top    = center_y + radius;

			/* 深度范围：子视锥 Z 跨度 ∪ 与该级联 xy 相交的投射物 Z 跨度。近端并入投射物是为了让
			 * 视锥外、靠光源一侧的遮挡物仍能投影进来，否则模型上半截的影子会丢。 */
			float ls_depth_z_min = ls_min.z;
			float ls_depth_z_max = ls_max.z;
			float geometry_z_min = std::numeric_limits<float>::max();
			float geometry_z_max = -std::numeric_limits<float>::max();

			/* 本轮要提交的投射物：与正交盒 xy 相交的那些。射不到盒子里的物体，画了也
			 * 不产生任何阴影，直接跳过 —— 层数越多省得越多。 */
			std::vector<uint32_t> cascade_casters;
			cascade_casters.reserve(caster_bounds.size());
			for (const auto& bounds : caster_bounds)
			{
				/* 光空间 xy 的 2D 相交测试。带一点余量：AABB 按绑定姿态算，蒙皮动画会把
				 * 顶点带到盒子外，不留余量影子会在级联边界闪 */
				const glm::vec3 margin = (bounds.Max - bounds.Min) * kCasterCullMargin;
				if (bounds.Max.x + margin.x < ortho_left || bounds.Min.x - margin.x > ortho_right)
					continue;
				if (bounds.Max.y + margin.y < ortho_bottom || bounds.Min.y - margin.y > ortho_top)
					continue;

				geometry_z_min = std::min(geometry_z_min, bounds.Min.z);
				geometry_z_max = std::max(geometry_z_max, bounds.Max.z);
				cascade_casters.push_back(bounds.ObjectIndex);
			}
			if (geometry_z_min <= geometry_z_max)
			{
				ls_depth_z_min = std::min(ls_depth_z_min, geometry_z_min);
				ls_depth_z_max = std::max(ls_depth_z_max, geometry_z_max);
			}

			/* 地形不参与 XY 拟合，但山体的影子会甩进画面，深度板必须盖住这一带的地形。
			 * 直接拿世界包围盒八个角在光空间取 AABB 求交，别反算世界矩形：太阳贴近地平线时
			 * z = 0 那张面近乎竖直，反算出的 xz 范围退化成一条线，地形会整片被近远平面裁掉。 */
			if (scene != nullptr)
			{
				for (auto [entity, terrain] : scene->GetRegistry().view<TerrainComponent>().each())
				{
					if (!terrain.m_CastShadow || !scene->IsEntityInRenderView(entity, culling_mask))
						continue;
					if (terrain.m_Field == nullptr)
						terrain.m_Field = CreateSharedPtr<TerrainField>();
					terrain.m_Field->Rebuild(terrain, scene->GetRenderWorldTransform(entity));
					if (terrain.m_Field->IsEmpty())
						continue;

					const glm::vec3 origin = terrain.m_Field->GetOrigin();
					const glm::vec2 half = terrain.m_Field->GetWorldHalfExtent();
					const float height_min = terrain.m_Field->GetMinWorldHeight();
					const float height_max = terrain.m_Field->GetMaxWorldHeight();

					glm::vec3 ls_min(std::numeric_limits<float>::max());
					glm::vec3 ls_max(-std::numeric_limits<float>::max());
					for (int corner = 0; corner < 8; ++corner)
					{
						const glm::vec3 world(
							origin.x + ((corner & 1) ? half.x : -half.x),
							(corner & 2) ? height_max : height_min,
							origin.z + ((corner & 4) ? half.y : -half.y));
						const glm::vec3 ls = glm::vec3(light_view_mat * glm::vec4(world, 1.0f));
						ls_min = glm::min(ls_min, ls);
						ls_max = glm::max(ls_max, ls);
					}

					/* 正交盒外的地形写不进这张图，跳过 */
					if (ls_max.x < ortho_left || ls_min.x > ortho_right
						|| ls_max.y < ortho_bottom || ls_min.y > ortho_top)
						continue;

					ls_depth_z_min = std::min(ls_depth_z_min, ls_min.z);
					ls_depth_z_max = std::max(ls_depth_z_max, ls_max.z);
				}
			}

			// near/far 由光照空间 Z 范围推导（视图空间 Z = -光照空间 Z）
			const float epsilon = 0.05f;
			float ortho_near = -ls_depth_z_max - epsilon; // 最近点（光照空间 Z 最大）
			float ortho_far  = -ls_depth_z_min + epsilon; // 最远点（光照空间 Z 最小）
			if (ortho_far <= ortho_near) ortho_far = ortho_near + 0.1f;

			glm::mat4 light_proj_mat = MakeReversedZProjection(glm::ortho(ortho_left, ortho_right, ortho_bottom, ortho_top, ortho_near, ortho_far));

			// 计算并设置光照视图投影矩阵
			glm::mat4 light_view_proj_mat = light_proj_mat * light_view_mat;
			m_CascadeShadowMaps[cascade_id]->SetLightViewProjectionMat(light_view_proj_mat);
			m_CascadeShadowMaps[cascade_id]->SetDepthRange(ortho_far - ortho_near);

			/* 拿不到可见网格（首次准备那趟）就把表失效，ShadowPass 退回全量提交 */
			if (mesh_objects != nullptr)
				m_CascadeShadowMaps[cascade_id]->SetCasterIndices(std::move(cascade_casters));
			else
				m_CascadeShadowMaps[cascade_id]->ClearCasterList();

			// 记录该级联远边界（视图空间距离），供着色阶段选择级联
			cascade_splits[cascade_id] = seg_far;
		}

		// 保存级联分割距离，供 AddShadowPass 填充光照 UBO
		m_CascadeSplits = cascade_splits;
	}

	/* 重算全部点光/聚光阴影面的视图投影矩阵。
	 * 光源位置 / 旋转 / 范围每帧可能变化，且单光源只有 1 / 6 个矩阵，直接重算不做脏检测。 */
	void ShadowMapManager::UpdatePunctualMatrices()
	{
		if (m_PunctualShadowMaps.empty())
			return;

		for (const auto& shadow_map : m_PunctualShadowMaps)
		{
			const auto light = std::dynamic_pointer_cast<PunctualLight>(shadow_map->m_pLight);
			if (!light)
				continue;

			/* 远平面 = 光源范围（与光照衰减同一份数据），超出范围的遮挡物不产生阴影 */
			const float far_plane = std::max(light->GetRange(), kPunctualShadowNearPlane + 0.01f);

			glm::mat4 light_view_mat;
			float fov_degrees = kPointLightFaceFov;
			if (light->GetLightType() == LightType::Point)
			{
				/* 点光：按面取固定朝向的 90° 视图矩阵（面顺序与采样端约定一致） */
				light_view_mat = ShadowMap::GetPunctualLightViewMatrix(shadow_map->m_FaceIndex, light->GetPosition());
			}
			else
			{
				/* 聚光：视图朝向 = 光传播方向，投影视锥全角 = 外锥角。
				 * 锥角接近 180° 时透视矩阵退化，夹取到安全范围。 */
				const auto spot = std::static_pointer_cast<SpotLight>(light);
				light_view_mat = ShadowMap::GetDirectionalLightViewMatrix(spot->GetDirection(), spot->GetPosition());
				fov_degrees = glm::clamp(spot->GetAngle(), 1.0f, 170.0f);
			}

			const glm::mat4 light_proj_mat = MakeReversedZProjection(
				glm::perspective(glm::radians(fov_degrees), 1.0f, kPunctualShadowNearPlane, far_plane));
			shadow_map->SetLightViewProjectionMat(light_proj_mat * light_view_mat);
		}
	}

	/* 取某光源的点光/聚光阴影数据（供光照阶段采样） */
	PunctualShadowData ShadowMapManager::GetPunctualShadowData(const Light* light) const
	{
		PunctualShadowData data;
		if (light == nullptr)
			return data;

		const auto* punctual = AsPunctualLight(light);
		if (punctual == nullptr)
			return data;

		for (const auto& shadow_map : m_PunctualShadowMaps)
		{
			if (shadow_map->m_pLight.get() != light)
				continue;

			/* 首次命中：定下基址与远平面（同一光源的所有面连续排列） */
			if (!data.Valid)
			{
				data.Valid = true;
				data.BaseLayer = shadow_map->GetLayer();
				data.Far = punctual->GetRange();
			}

			data.FaceMat[shadow_map->m_FaceIndex] = shadow_map->GetLightViewProjectionMat();
			data.FaceCount = std::max<uint32_t>(data.FaceCount, shadow_map->m_FaceIndex + 1u);
		}
		return data;
	}

	/* 本视图负责主阴影的光源下标（口径见头文件）：级联方向光优先，其次点光 / 聚光；
	 * 完全没有阴影数据时返回 0，调用方按"照旧由第一笔合成"处理 */
	size_t ShadowMapManager::FindPrimaryShadowLightIndex(const std::vector<SharedPtr<Light>>& lights) const
	{
		if (lights.empty())
			return 0;

		if (!m_CascadeShadowMaps.empty())
		{
			const Light* cascade_light = m_CascadeShadowMaps[0]->GetLight().get();
			for (size_t index = 0; index < lights.size(); ++index)
			{
				if (lights[index].get() == cascade_light)
					return index;
			}
		}

		for (size_t index = 0; index < lights.size(); ++index)
		{
			if (GetPunctualShadowData(lights[index].get()).Valid)
				return index;
		}
		return 0;
	}

	/* 清空已收集的阴影贴图与纹理，保留Manager对象本身 */
	void ShadowMapManager::Reset()
	{
		m_CascadeShadowMaps.clear();
		m_PunctualShadowMaps.clear();
		m_ShadowMapTexture = nullptr;
		m_RequiredTextureDesc = {};
		m_MaxDimension = 128;
		/* 清空后缓存失效，下次 UpdateCascadeMatrices 必须重算 */
		m_IsDirty = true;
		m_LastLightDir = glm::vec3(0.0f);
		m_LastViewMat = glm::mat4(1.0f);
		m_LastProjMat = glm::mat4(1.0f);
		m_LastCasterCount = 0;
	}

	/* 准备阴影纹理 */
	void ShadowMapManager::PrepareRequiredTexture()
	{
		uint8_t layer = 0;
		uint32_t max_dimension = 0;

		/* 方向光 */
		for (auto& shadow_map : m_CascadeShadowMaps)
		{
			shadow_map->SetLayer(layer++);
			max_dimension = std::max(max_dimension, shadow_map->m_pLight->GetShadowMapInfo()->Size);
		}

		/* 点光/聚光（与级联共用同一张纹理数组，尺寸统一取最大值） */
		for (auto& shadow_map : m_PunctualShadowMaps)
		{
			shadow_map->SetLayer(layer++);
			max_dimension = std::max(max_dimension, shadow_map->m_pLight->GetShadowMapInfo()->Size);
		}

		const uint8_t total_layer_num = layer;
		if (total_layer_num == 0)
			return;

		/* 这里只记录阴影纹理数组的描述，实际纹理由 FrameGraph 在 AddShadowPass 里创建。别在这创建
		 * m_ShadowMapTexture：它会被上一帧 ShadowPass 的 FrameBuffer 绑着、延迟回收，glGenTextures 复用
		 * 同一 id 后再 glTexStorage3D 会报 "Texture is immutable"。深度格式用 Depth32F（跟 Reversed-Z
		 * 搭配无定点量化损失，OpenGL 3.0+ 也保证它能作深度附件渲染）。 */
		m_RequiredTextureDesc = { max_dimension, total_layer_num, 1, TextureFormat::Depth32F };
	}

	/* 创建全零深度数组，供不使用当前视图阴影的 Pass 采样。 */
	void ShadowMapManager::AddNoShadowMapPass(FrameGraph& frame_graph)
	{
		struct NoShadowPassData
		{
			FrameGraphResourceHandleTyped<FrameGraphTexture> Texture;
		};

		auto pass = frame_graph.AddPass<NoShadowPassData>("NoShadowMapPass",
			[](FrameGraphBuilder& builder, NoShadowPassData& data)
			{
				FrameGraphTexture::Descriptor texture_desc;
				texture_desc.Width = 1;
				texture_desc.Height = 1;
				texture_desc.Depth = kFallbackCascadeCount;
				texture_desc.Samples = 1;
				texture_desc.TextureFormat = TextureFormat::Depth32F;
				texture_desc.SamplerType = SamplerType::Sampler2DArray;
				data.Texture = builder.CreateTexture("NoShadowMap", texture_desc);
				builder.BindOutputResource(data.Texture,
					FrameGraphTexture::Usage::DepthAttachment | FrameGraphTexture::Usage::Sampleable);

				FrameGraphPassInfo::Descriptor pass_desc;
				pass_desc.Attachments.DepthAttachment() = data.Texture;
				pass_desc.ViewportRegion = { 0, 0, 1, 1 };
				builder.CreateRenderPass("NoShadowMapRenderTarget", pass_desc);
			},
			[](const FrameGraphResources& resources, const NoShadowPassData& data)
			{
				const auto framebuffer = resources.GetPassRenderTarget();
				RenderRasterState clear_state;
				clear_state.EnableDepthWrite = true;
				Renderer::GetRenderAPI()->ApplyRasterState(clear_state);
				for (uint16_t layer = 0; layer < kFallbackCascadeCount; ++layer)
				{
					framebuffer->Bind(FrameBufferBindInfo::ToDepthLayer(layer));
					Renderer::SetViewport(0, 0, 1, 1);
					Renderer::GetRenderAPI()->SetScissor(0, 0, 1, 1);
					Renderer::Clear();
					framebuffer->Unbind();
				}
			});

		frame_graph.GetBlackboard()["NoShadowMapHandle"] = pass->GetData().Texture;
	}

	/* 阴影图数组各层的显示名（下标 = 数组层号）：级联在前、点光/聚光在后 —— 与
	 * PrepareRequiredTexture 的层分配同序，给 ShadowPass 的 Debug 组和抓取逐层条目共用 */
	std::vector<std::string> ShadowMapManager::GetShadowLayerNames() const
	{
		std::vector<std::string> layer_names(m_CascadeShadowMaps.size() + m_PunctualShadowMaps.size());
		for (const auto& shadow_map : m_CascadeShadowMaps)
			layer_names[shadow_map->GetLayer()] = "Cascade " + std::to_string(shadow_map->GetLayer());
		for (const auto& shadow_map : m_PunctualShadowMaps)
			layer_names[shadow_map->GetLayer()] = "Punctual " + std::to_string(shadow_map->GetLayer());
		return layer_names;
	}

	/* 将ShadowPass注入到FrameGraph */
	void ShadowMapManager::AddShadowPass(FrameGraph& frame_graph, const SharedPtr<Scene>& scene, RenderView* render_view)
	{
		const bool has_any_shadow_map = !m_CascadeShadowMaps.empty() || !m_PunctualShadowMaps.empty();

		/* 若尚未准备（如FrameGraph setup阶段早于RenderView每帧的PrepareLights），
		 * 则先基于当前Scene准备阴影纹理描述，确保setup阶段能创建正确尺寸的纹理资源
		 */
		if (!has_any_shadow_map)
			PrepareForShadowMaps(scene, render_view ? render_view->GetCullingCamera() : nullptr);

		if (m_CascadeShadowMaps.empty() && m_PunctualShadowMaps.empty())
			return;

		/* 各层显示名（Debug 组 / 抓取逐层条目共用的单一来源）：把数组按层注册给抓取设施，
		 * Frame Graph 页据此把阴影图按层展示（Cascade N / Punctual N） */
		const std::vector<std::string> layer_names = GetShadowLayerNames();
		frame_graph.GetCapture().SetLayerNames(kShadowMapTextureName, layer_names);

		/* ========== Pass: Shadow Pass ========== */
		struct ShadowPassData
		{
			FrameGraphResourceHandleTyped<FrameGraphTexture> ShadowMapHandle;
		};

		auto shadow_pass = frame_graph.AddPass<ShadowPassData>("ShadowPass",
			[this](FrameGraphBuilder& builder, ShadowPassData& data)
			{
				/* 根据已准备的阴影纹理描述创建FrameGraph纹理资源 */
				FrameGraphTexture::Descriptor shadow_tex_desc;
				shadow_tex_desc.Width = m_RequiredTextureDesc.Size;
				shadow_tex_desc.Height = m_RequiredTextureDesc.Size;
				shadow_tex_desc.Depth = m_RequiredTextureDesc.LayerNum; // 数组层数（级联数）
				shadow_tex_desc.MipLevels = m_RequiredTextureDesc.MipLevels;
				shadow_tex_desc.TextureFormat = m_RequiredTextureDesc.Format;
				shadow_tex_desc.SamplerType = SamplerType::Sampler2DArray;
				data.ShadowMapHandle = builder.CreateTexture(kShadowMapTextureName, shadow_tex_desc);
				builder.BindOutputResource(data.ShadowMapHandle, FrameGraphTexture::Usage::DepthAttachment | FrameGraphTexture::Usage::Sampleable);

				FrameGraphPassInfo::Descriptor pass_desc;
				pass_desc.Attachments.DepthAttachment() = data.ShadowMapHandle;
				pass_desc.ViewportRegion = { 0, 0, m_RequiredTextureDesc.Size, m_RequiredTextureDesc.Size };
				builder.CreateRenderPass("ShadowPassRenderTarget", pass_desc);
			},
			[this, render_view, layer_names](const FrameGraphResources& resources, const ShadowPassData& data)
			{
				const auto render_pass_info = resources.GetPassRenderTarget();
				render_view->EmplacePassFrameBuffer("ShadowPass", render_pass_info);

				auto render_api = Renderer::GetRenderAPI();
				const uint32_t shadow_size = m_RequiredTextureDesc.Size;

				/* 把源材质的数值参数抄进投影材质：贴图不需要，只要叶形与压弯这些系数 */
				const auto copy_material_values = [](const Material& source, Material& target)
				{
					for (const auto& [key, info] : source.GetAllParameters())
					{
						(void)key;
						if (info.Type == ParamType::Texture)
							continue;
						target.SetParameters(info.Type, info.Name, info.Value);
					}
				};

				/* 草的投影：模拟投射物列表之外单独提交一笔。远近只做粗筛，疏密交给着色器按株抖点，
				 * 走的是跟可见着色同一套叶片展开，所以风和脚印都对得上 */
				const auto submit_grass_casters = [this, render_view, copy_material_values](
					uint8_t cascade_id, const glm::mat4& light_view_projection)
				{
					if (m_GrassShadowShader == nullptr)
						return;

					const SharedPtr<Scene> scene = render_view->GetOwnerScene();
					if (scene == nullptr)
						return;

					const Camera* camera = render_view->GetCullingCamera();
					const glm::vec3 eye = camera != nullptr ? camera->GetPosition() : glm::vec3(0.0f);

					/* 草影最远距离默认跟阴影距离走：主方向光的 ShadowFar，没设就退回相机 far */
					float shadow_far = (camera != nullptr) ? camera->GetFarClip() : 60.0f;
					if (!m_CascadeShadowMaps.empty() && m_CascadeShadowMaps[0]->m_pLight != nullptr)
					{
						const auto& info = m_CascadeShadowMaps[0]->m_pLight->GetShadowMapInfo();
						if (info != nullptr && info->ShadowFar > 0.0f)
							shadow_far = info->ShadowFar;
					}

					const auto grass_material = Material::Create(m_GrassShadowShader);
					grass_material->SetParameters(ParamType::Int, "u_CascadeIndex", static_cast<int>(cascade_id));
					grass_material->SetParameters(ParamType::Vec3, "u_GrassShadowEye", eye);
					/* 叶片是单面几何，投影不能剔背面，否则一半的草不写深度 */
					grass_material->GetRasterState().CullMode = CullMode::Cull_None;

					const MeshPrimitive& blade = GrassField::GetBladePrimitive();
					/* 只画落进本级联正交盒的块：盒外的草写不进这张阴影图，
					 * 顶点却照跑一遍。用光源矩阵的视锥判，块是轴对齐盒 */
					const Frustum light_frustum = Frustum::FromViewProjection(light_view_projection);
					for (auto [entity, grass] : scene->GetRegistry().view<GrassComponent>().each())
					{
						if (!grass.m_CastShadow || grass.GetMaterial() == nullptr
							|| grass.m_Field == nullptr || grass.m_Field->IsEmpty())
							continue;
						if (!scene->IsEntityInRenderView(entity, render_view->GetCullingMask()))
							continue;

						const auto& instance_buffer = grass.m_Field->GetInstanceBuffer();
						if (instance_buffer == nullptr)
							continue;

						/* 影子要跟草长得一模一样：把源材质的数值参数（叶形弯曲、压弯强度这些）整份搬过来。
						 * 只有风与脚印走 per-draw，其余从材质来 */
						copy_material_values(*grass.GetMaterial(), *grass_material);

						/* 草影抽稀半径：默认跟阴影距离，组件上可手动收紧（取较小） */
						const float fade_radius = GrassDraw::ShadowFadeRadius(grass, shadow_far);
						grass_material->SetParameters(ParamType::Float, "u_GrassShadowFadeRadius", fade_radius);
						grass_material->SetParameters(ParamType::Float, "u_GrassShadowFullRadius",
							GrassDraw::ShadowFullRadius(fade_radius));

						DrawParams per_draw = GrassDraw::MakeDrawParams(grass);

						/* 这一档级联已经按光源视锥剔好：逐段提交，不再回 CPU 按批挑。
						 * 着色器自己的抽稀这时要关掉，否则同一株被抽两次 */
						const uint32_t slot = static_cast<uint32_t>(cascade_id) + 1u;
						if (grass.m_Field->CanDrawIndirect(render_view, slot, scene->GetFrameIndex()))
						{
							grass_material->SetParameters(ParamType::Int, "u_GrassShadowGpuCulled", 1);
							const uint32_t segments = grass.m_Field->GetIndirectSegments(render_view, slot);
							const uint32_t capacity = grass.m_Field->GetIndirectSegmentCapacity(render_view, slot);
							const uint32_t args_stride = GrassField::kIndirectArgsStride
								* static_cast<uint32_t>(sizeof(uint32_t));
							const uint32_t instance_stride = static_cast<uint32_t>(sizeof(GrassInstanceData));
							for (uint32_t segment = 0; segment < segments; ++segment)
							{
								Renderer::SubmitIndirect(grass_material, blade,
									grass.m_Field->GetIndirectDrawBuffer(render_view, slot),
									grass.m_Field->GetIndirectArgs(render_view, slot), &per_draw,
									&Renderer::GetGrassInstanceLayout(), segment * args_stride,
									segment * capacity * instance_stride);
								RenderStatistics::Instance().AddGrassDrawCall(RenderStatistics::GrassPass::Shadow);
							}
							continue;
						}
						grass_material->SetParameters(ParamType::Int, "u_GrassShadowGpuCulled", 0);

						/* 没剔成就按批提交。批比块大，盒外与远处的判定放宽了，
						 * 多出来的株在着色器里按距离抽稀丢掉，不写深度 */
						const auto& batches = grass.m_Field->GetBatches();
						const bool use_batches = GrassField::IsGlobalBatchingEnabled() && !batches.empty();
						if (use_batches)
						{
							for (const GrassField::Batch& batch : batches)
							{
								if (light_frustum.IsOutside(batch.Center, batch.Extent))
									continue;

								const float batch_distance = glm::distance(eye, batch.Center)
									- glm::length(glm::vec2(batch.Extent.x, batch.Extent.z));
								if (batch_distance > fade_radius)
									continue;

								Renderer::SubmitInstanced(grass_material, blade, instance_buffer,
									batch.First, batch.Count, &per_draw, 0, &Renderer::GetGrassInstanceLayout());
								RenderStatistics::Instance().AddGrassDrawCall(RenderStatistics::GrassPass::Shadow);
							}
							continue;
						}

						for (const GrassField::Chunk& chunk : grass.m_Field->GetChunks())
						{
							if (chunk.Count == 0)
								continue;

							/* 盒外整块跳过：级联盒外的草写不进这张阴影图，顶点却照跑一遍。
							 * 盒内再按距离圈收：圈外的块不用提交 */
							if (light_frustum.IsOutside(chunk.Center, chunk.Extent))
								continue;

							/* 按块的外接半径放宽筛选：块中心在圈外但叶子还伸进圈里的照样提交 */
							const float distance = glm::distance(eye, chunk.Center)
								- glm::length(glm::vec2(chunk.Extent.x, chunk.Extent.z));
							if (distance > fade_radius)
								continue;

							Renderer::SubmitInstanced(grass_material, blade, instance_buffer,
								chunk.First, chunk.Count, &per_draw, 0, &Renderer::GetGrassInstanceLayout());
							RenderStatistics::Instance().AddGrassDrawCall(RenderStatistics::GrassPass::Shadow);
						}
					}
				};

				/* 每层的公共渲染体：清深度 + 提交本层投射物（级联按各自的剔除表，点光/聚光全量）。
				 * draw_grass 只对方向光级联打开：草的投影按级联矩阵走，点光层没有对应矩阵 */
				const auto render_shadow_layer = [&](uint8_t layer, const SharedPtr<Material>& shadow_material,
					const std::vector<uint32_t>* caster_indices, bool draw_grass,
					const glm::mat4* light_view_projection)
				{
					/* 绑定FrameBuffer为指定层（统一 Bind 接口，自动设置 viewport 并按层附着 Depth 附件） */
					render_pass_info->Bind(FrameBufferBindInfo::ToDepthLayer(layer));
					render_api->SetViewport(0, 0, shadow_size, shadow_size);
					render_api->SetScissor(0, 0, shadow_size, shadow_size);
					render_api->Clear();

					/* 投射物按剔除方式和网格分组：同组连着画，管线和顶点缓冲都不换 */
					const auto caster_list = RenderSorting::CollectShadowCasterList(
						render_view->GetShadowCasterObjects(), shadow_material->GetRasterState(), caster_indices);

					/* 组批：列表按剔除方式 + 顶点数组排过序，同一组的投射物合成一笔实例化绘制 */
					struct CasterBatch
					{
						SharedPtr<DeviceVertexArray> VertexArray;
						PrimitiveType Type{ PrimitiveType::Triangles };
						RenderRasterState State{};
						uint32_t First{ 0 };
						uint32_t Count{ 0 };
					};

					std::vector<ObjectInstanceData> instances;
					std::vector<CasterBatch> batches;
					instances.reserve(caster_list.size());
					batches.reserve(8);

					for (const VisibleMeshObject* mesh_object : caster_list)
					{
						if (mesh_object->MeshSegment == nullptr)
							continue;

						/* 剔除方式跟物体材质走：单面材质只画正面，背面不用光栅化；
						 * 双面材质（Cull_None）两面都写，和主视图看到的是同一侧表面 */
						RenderRasterState caster_state = shadow_material->GetRasterState();
						if (mesh_object->Material != nullptr)
						{
							const auto& source_state = mesh_object->Material->GetRasterState();
							caster_state.CullMode = source_state.CullMode;
							caster_state.FrontFaceType = source_state.FrontFaceType;
						}

						const MeshPrimitive& primitive = mesh_object->MeshSegment->GetMeshPrimitive();
						if (batches.empty() || batches.back().VertexArray != primitive.VertexArray
							|| batches.back().Type != primitive.PrimitiveType
							|| batches.back().State.CullMode != caster_state.CullMode
							|| batches.back().State.FrontFaceType != caster_state.FrontFaceType)
						{
							CasterBatch batch;
							batch.VertexArray = primitive.VertexArray;
							batch.Type = primitive.PrimitiveType;
							batch.State = caster_state;
							batch.First = static_cast<uint32_t>(instances.size());
							batches.push_back(std::move(batch));
						}

						ObjectInstanceData instance;
						instance.Local2World = mesh_object->Local2WorldMat;
						instances.push_back(instance);
						++batches.back().Count;
					}

					if (!instances.empty())
					{
						const auto range = Renderer::UploadObjectInstances(instances.data(),
							static_cast<uint32_t>(instances.size()));

						for (const CasterBatch& batch : batches)
						{
							DrawParams caster_draw;
							caster_draw.RasterStateOverride = batch.State;
							Renderer::SubmitInstanced(shadow_material, MeshPrimitive{ batch.VertexArray, batch.Type },
								range.Buffer, range.FirstInstance + batch.First, batch.Count, &caster_draw);
						}
					}

					/* 草不在投射物列表里：在本层的渲染通道里单独补一笔 */
					if (draw_grass && light_view_projection != nullptr)
						submit_grass_casters(layer, *light_view_projection);

					/* 地形同样不在投射物列表里：patch 按这一层的盒子选，单独补一笔 */
					if (light_view_projection != nullptr)
					{
						if (const SharedPtr<Scene> scene = render_view->GetOwnerScene())
							TerrainDraw::SubmitShadow(*render_view, *scene, *light_view_projection, layer);
					}

					render_pass_info->Unbind();
				};

				/* 阴影投射材质：自身保持默认单面，逐物体的剔除方式在提交时按源材质覆盖 */
				const auto make_caster_material = [this]()
				{
					return Material::Create(m_ShadowCasterShader);
				};


				/* 渲染每级联阴影，复用UpdateCascadeMatrices已计算好的VP矩阵与Layer */
				if (!m_CascadeShadowMaps.empty())
				{
					/* 把级联矩阵和深度跨度写进光照 UBO：阴影 Pass 按级联索引取矩阵，
					 * LightingPass 按视距选级联 */
					Renderer::FillLightUniformBuffer(m_CascadeShadowMaps[0]->m_pLight, this);

					/* 只画宿主方向光的级联：容器里可能还挂着别的方向光的层（级联只有一套，
					 * 它们没被拟合，矩阵是单位阵），画了也只会写进没人采样的层 */
					const Light* host_light = m_CascadeShadowMaps[0]->m_pLight.get();
					for (const auto& cascade_shadow_map : m_CascadeShadowMaps)
					{
						if (cascade_shadow_map->m_pLight.get() != host_light)
							continue;

						const uint8_t cascade_id = cascade_shadow_map->GetLayer();
						PROFILE_GPU_SCOPE(layer_names[cascade_id]);

						auto shadow_material = make_caster_material();
						shadow_material->SetParameters(ParamType::Int, "u_CascadeIndex", static_cast<int>(cascade_id));

						/* 只提交与该级联正交盒相交的投射物；表没备好时退回全量 */
						const glm::mat4 light_view_projection = cascade_shadow_map->GetLightViewProjectionMat();
						render_shadow_layer(cascade_id, shadow_material,
							cascade_shadow_map->HasCasterList() ? &cascade_shadow_map->GetCasterIndices() : nullptr,
							true, &light_view_projection);
					}
				}

				/* 渲染点光 / 聚光阴影层：单面矩阵作为材质参数下发，u_CascadeIndex = -1 让 ShadowCaster.glsl
				 * 走单矩阵分支。注意：参数名必须跟 ShadowCaster.glsl 的裸 uniform 一致（u_ShadowPassMat），
				 * 也不能跟 LightUniformBuffer 成员重名（撞名编译不过）。 */
				const auto& casters = render_view->GetShadowCasterObjects();
				std::vector<uint32_t> face_casters;
				for (const auto& punctual_shadow_map : m_PunctualShadowMaps)
				{
					const uint8_t layer = punctual_shadow_map->GetLayer();
					PROFILE_GPU_SCOPE(layer_names[layer]);

					auto shadow_material = make_caster_material();
					shadow_material->SetParameters(ParamType::Int, "u_CascadeIndex", -1);
					shadow_material->SetParameters(ParamType::Mat4, "u_ShadowPassMat",
						punctual_shadow_map->GetLightViewProjectionMat());

					/* 只提交落在这一面视锥里的投射物：点光六个面各自只看得到一部分 */
					const Frustum face_frustum = Frustum::FromViewProjection(
						punctual_shadow_map->GetLightViewProjectionMat());
					face_casters.clear();
					for (uint32_t index = 0; index < static_cast<uint32_t>(casters.size()); ++index)
					{
						if (!face_frustum.IsOutside(casters[index]))
							face_casters.push_back(index);
					}
					render_shadow_layer(layer, shadow_material, &face_casters, false, nullptr);
				}
			});

		/* 将阴影贴图句柄存储到Blackboard，供后续Pass使用 */
		frame_graph.GetBlackboard().Add("ShadowMapHandle", shadow_pass->GetData().ShadowMapHandle);
	}

}