#include "Pch.h"
#include "ShadowMap.h"
#include "Scene.h"
#include <cmath>
#include <glm/gtc/type_ptr.hpp>
#include <Helios/Common/Math.h>
#include <Helios/VirtualDevice/DeviceTexture.h>
#include <Helios/Renderer/FrameGraph/FrameGraph.h>
#include <Helios/Renderer/RenderView.h>
#include <Helios/Renderer/Renderer.h>
#include <Helios/Scene/Material.h>
#include <Helios/Application/AssetManager.h>
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
		constexpr uint32_t kFallbackCascadeCount = 4;

		/* 点光/聚光阴影投影的近平面：取足够小的值以容纳贴着光源的几何；
		 * 深度用 Depth32F + Reversed-Z（近处精度高），小近平面不产生可见失真。 */
		constexpr float kPunctualShadowNear = 0.05f;

		/* 点光立方体各面视锥的半角（90° 全角） */
		constexpr float kPointLightFaceFov = 90.0f;
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
			m_ShadowCasterShader = ShaderAssetManager::Instance().GetOrLoad(ABSOLUTE_PATH("Shaders/Shadow.glsl"));

		PrepareRequiredTexture();

		/* 准备方向光 ShadowMap：算级联 VP 矩阵和分割距离。这里拿不到可见网格列表，先按相机
		 * 子视锥拟合一次；RenderView::Execute 随后会带着投射物重新拟合（脏检测把投射物输入
		 * 也算上），保证投射物不被 near / far 板裁掉。 */
		UpdateCascadeMatrices(camera, nullptr);
	}

	/* 依据当前方向光方向、相机与场景投射物，重算各级联的视图投影矩阵与分割距离。
	 * 供 PrepareForShadowMaps（首次准备）与每帧执行前（方向光旋转 / 相机移动）共用。
	 * 通过对比缓存（方向光方向 + 相机视图/投影矩阵 + 是否拿到投射物）跳过无变化的重算。 */
	void ShadowMapManager::UpdateCascadeMatrices(const Camera* camera, const std::vector<VisibleMeshObject>* mesh_objects)
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
		/* 是否拿到了投射物包围盒：它参与光源视锥拟合，属于拟合输入的一部分 */
		const bool has_casters = mesh_objects != nullptr && !mesh_objects->empty();
		const bool unchanged =
			!m_IsDirty &&
			dir_equal &&
			mat_equal(view_mat, m_LastViewMat) &&
			mat_equal(proj_mat, m_LastProjMat) &&
			has_casters == m_LastFitHadCasters;
		if (unchanged)
			return;

		/* 记录本次用于计算的输入，供下次对比 */
		m_LastLightDir = direction;
		m_LastViewMat = view_mat;
		m_LastProjMat = proj_mat;
		m_LastFitHadCasters = has_casters;
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

		std::vector<float> split_dist(cascade_cnt + 1);
		split_dist[0] = cam_near;
		for (uint8_t i = 1; i <= cascade_cnt; ++i)
		{
			const float f = static_cast<float>(i) / static_cast<float>(cascade_cnt);
			const float log_split = cam_near * std::pow(cam_far / cam_near, f);
			const float uni_split = cam_near + (cam_far - cam_near) * f;
			split_dist[i] = lambda * log_split + (1.0f - lambda) * uni_split;
		}

		/* 收集场景几何体在光照空间的 AABB（每个 MeshSegment 一个）。级联的深度板不能只看相机
		 * 子视锥：空气和近相机那一段会把 z 跨度撑大，深度图偏黑、精度变差；所以要按"与该级联 xy
		 * 相交的几何体"来拟合 near / far。 */
		struct LightSpaceBounds { glm::vec3 Min; glm::vec3 Max; };
		std::vector<LightSpaceBounds> caster_bounds;
		if (mesh_objects)
		{
			caster_bounds.reserve(mesh_objects->size());
			for (const auto& mesh_object : *mesh_objects)
			{
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
				}
				caster_bounds.emplace_back(bounds);
			}
		}

		// 相机世界矩阵（用于把视图空间子视锥角点变换回世界空间）；view_mat 已在脏检测处获取
		const glm::mat4 cam_world_mat = glm::inverse(view_mat);

		/* 全部投射物在光照空间的并集 AABB：级联范围收敛用（见下方逐级联求交）。
		 * "投射物"即全部可见网格 —— 阴影的接收面也来自它们，因此并集必然包住
		 * 所有需要采样的像素；任何能遮挡这些像素的物体也都在并集内。 */
		glm::vec3 caster_union_min(std::numeric_limits<float>::max());
		glm::vec3 caster_union_max(-std::numeric_limits<float>::max());
		const bool has_caster_union = !caster_bounds.empty();
		for (const auto& bounds : caster_bounds)
		{
			caster_union_min = glm::min(caster_union_min, bounds.Min);
			caster_union_max = glm::max(caster_union_max, bounds.Max);
		}

		// 透视相机视锥参数（用于构造子视锥角点）；非透视（正交）相机退化为整段
		const float fov        = camera ? camera->GetFov() : 45.0f;
		const float aspect     = camera ? camera->GetAspectRatio() : 1.0f;
		const float tan_half_fov = std::tan(glm::radians(fov) * 0.5f);

		// 各级联在视图空间的分割距离（远边界），写回 UBO 供着色阶段选级联
		glm::vec4 cascade_splits(0.0f);

		// 为每级联计算投影矩阵：取该段子视锥的 8 个角点 → 世界 → 光照空间 → AABB → tight-fit 正交
		for (uint8_t cascade_id = 0; cascade_id < cascade_cnt; ++cascade_id)
		{
			const float seg_near = split_dist[cascade_id];
			const float seg_far  = split_dist[cascade_id + 1];

			// 子视锥在视图空间（相机看向 -Z）的 8 个角点
			const float half_h_near = seg_near * tan_half_fov;
			const float half_w_near = half_h_near * aspect;
			const float half_h_far  = seg_far  * tan_half_fov;
			const float half_w_far  = half_h_far * aspect;

			glm::vec3 corners[8];
			// 近平面 4 角（z = -seg_near）
			corners[0] = glm::vec3(-half_w_near, -half_h_near, -seg_near);
			corners[1] = glm::vec3( half_w_near, -half_h_near, -seg_near);
			corners[2] = glm::vec3( half_w_near,  half_h_near, -seg_near);
			corners[3] = glm::vec3(-half_w_near,  half_h_near, -seg_near);
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

			/* tight-fit 基础范围：子视锥在光照空间的真实 AABB；别用 CascadeRadius 去钳（几何体会被
			 * 投到 [-1,1] 外写不进阴影图），那个值只当最小半边长下限。级联范围收敛：XY 与投射物并集求交，
			 * 没有投射物或不相交时退回子视锥拟合。 */
			bool range_clamped_to_casters = false;
			if (has_caster_union)
			{
				const glm::vec3 clipped_min = glm::max(ls_min, caster_union_min);
				const glm::vec3 clipped_max = glm::min(ls_max, caster_union_max);
				if (clipped_min.x <= clipped_max.x && clipped_min.y <= clipped_max.y)
				{
					ls_min.x = clipped_min.x;
					ls_max.x = clipped_max.x;
					ls_min.y = clipped_min.y;
					ls_max.y = clipped_max.y;
					range_clamped_to_casters = true;
				}
			}

			const float radius = shadow_map_info->CascadeRadius[cascade_id];

			// 居中并取对称正方形范围（以较长边为准），提升 PCF 采样稳定性
			const float center_x = (ls_min.x + ls_max.x) * 0.5f;
			const float center_y = (ls_min.y + ls_max.y) * 0.5f;
			const float half_x = (ls_max.x - ls_min.x) * 0.5f;
			const float half_y = (ls_max.y - ls_min.y) * 0.5f;
			float half_extent = std::max(half_x, half_y);
			// 下限：保证范围不小于级联半径（仅放大、不缩小真实 AABB）
			if (radius > 0.0f)
				half_extent = std::max(half_extent, radius);

			/* 收敛生效时加 2 纹素边距：PCF 邻接采样与光栅化边界不被卷积截断 */
			if (range_clamped_to_casters)
			{
				const float texel_est = (2.0f * half_extent) / static_cast<float>(shadow_map_info->Size);
				half_extent += 2.0f * texel_est;
			}

			float ortho_left   = center_x - half_extent;
			float ortho_right  = center_x + half_extent;
			float ortho_bottom = center_y - half_extent;
			float ortho_top    = center_y + half_extent;

			/* 纹素对齐：把"范围尺寸"向上量化到整纹素，中心吸附到纹素网格（正交视锥保持正方形、
			 * PCF 更均匀）；light view 不含相机信息，网格稳定，相机移动时阴影不抖。 */
			const float texel_size = (2.0f * half_extent) / static_cast<float>(shadow_map_info->Size);
			if (texel_size > 0.0f)
			{
				half_extent = (std::floor(half_extent / texel_size) + 1.0f) * texel_size;
				const float snapped_center_x = std::round(center_x / texel_size) * texel_size;
				const float snapped_center_y = std::round(center_y / texel_size) * texel_size;

				ortho_left   = snapped_center_x - half_extent;
				ortho_right  = snapped_center_x + half_extent;
				ortho_bottom = snapped_center_y - half_extent;
				ortho_top    = snapped_center_y + half_extent;
			}

			/* 深度范围：与该级联 xy 相交的几何体的 Z 跨度；没收敛时再并入相机子视锥的 Z 跨度
			 * （免得 near / far 板把物体切平）。收敛生效后几何体跨度就是完整依据，深度板收紧之后
			 * Depth16 对场景深度的分辨率提升很大。 */
			float ls_depth_z_min = ls_min.z;
			float ls_depth_z_max = ls_max.z;
			float geometry_z_min = std::numeric_limits<float>::max();
			float geometry_z_max = -std::numeric_limits<float>::max();
			for (const auto& bounds : caster_bounds)
			{
				/* 光空间 xy 的 2D 相交测试 */
				if (bounds.Max.x < ortho_left || bounds.Min.x > ortho_right)
					continue;
				if (bounds.Max.y < ortho_bottom || bounds.Min.y > ortho_top)
					continue;

				geometry_z_min = std::min(geometry_z_min, bounds.Min.z);
				geometry_z_max = std::max(geometry_z_max, bounds.Max.z);
			}
			const bool has_intersecting_geometry = geometry_z_min <= geometry_z_max;
			if (has_intersecting_geometry)
			{
				if (range_clamped_to_casters)
				{
					/* 收敛生效：几何体跨度即完整依据（接收面与遮挡物都在其中），
					 * 不再并入子视锥跨度 —— 避免把包围盒外的空白空气算进深度板。 */
					ls_depth_z_min = geometry_z_min;
					ls_depth_z_max = geometry_z_max;
				}
				else
				{
					ls_depth_z_min = std::min(ls_depth_z_min, geometry_z_min);
					ls_depth_z_max = std::max(ls_depth_z_max, geometry_z_max);
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
			const float far_plane = std::max(light->GetRange(), kPunctualShadowNear + 0.01f);

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
				glm::perspective(glm::radians(fov_degrees), 1.0f, kPunctualShadowNear, far_plane));
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
		m_LastFitHadCasters = false;
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
				data.ShadowMapHandle = builder.CreateTexture("ShadowMap", shadow_tex_desc);
				builder.BindOutputResource(data.ShadowMapHandle, FrameGraphTexture::Usage::DepthAttachment | FrameGraphTexture::Usage::Sampleable);

				FrameGraphPassInfo::Descriptor pass_desc;
				pass_desc.Attachments.DepthAttachment() = data.ShadowMapHandle;
				pass_desc.ViewportRegion = { 0, 0, m_RequiredTextureDesc.Size, m_RequiredTextureDesc.Size };
				builder.CreateRenderPass("ShadowPassRenderTarget", pass_desc);
			},
			[this, render_view](const FrameGraphResources& resources, const ShadowPassData& data)
			{
				const auto render_pass_info = resources.GetPassRenderTarget();
				render_view->EmplacePassFrameBuffer("ShadowPass", render_pass_info);

				auto render_api = Renderer::GetRenderAPI();
				const uint32_t shadow_size = m_RequiredTextureDesc.Size;

				/* 每层的公共渲染体：清深度 + 全量投射物提交（级联与点光/聚光共用） */
				const auto render_shadow_layer = [&](uint8_t layer, const SharedPtr<Material>& shadow_material)
				{
					/* 绑定FrameBuffer为指定层（统一 Bind 接口，自动设置 viewport 并按层附着 Depth 附件） */
					render_pass_info->Bind(FrameBufferBindInfo::ToDepthLayer(layer));
					render_api->SetViewport(0, 0, shadow_size, shadow_size);
					render_api->SetScissor(0, 0, shadow_size, shadow_size);
					render_api->Clear();

					for (const auto& mesh_object : render_view->GetVisibleMeshObjects())
					{
						/* 天空盒不产生投影：顶点着色器把它铺满全屏（z = 远平面），
						 * 如果参与投射，整个深度图会被压成一层"天空" */
						if (mesh_object.Material && mesh_object.Material->IsSkyBox())
							continue;

						/* 模型关闭「投影」后不写入阴影图（仍可接受阴影）：级联与点状共用这条 */
						if (!mesh_object.CastShadow)
							continue;

						Renderer::FillObjectUniformBuffer(mesh_object);
						Renderer::Submit(shadow_material, mesh_object.MeshSegment->GetMeshPrimitive());
					}

					render_pass_info->Unbind();
				};

				/* 阴影投射材质：关掉背面剔除 —— 单面几何（地面 / 薄墙 / 植被）必须能写进深度，免得绕序
				 * 约定有差异时这些 caster 直接消失；闭合几何体双面渲染的最终深度跟单面一致，只多花一点光栅化。 */
				const auto make_caster_material = [this]()
				{
					auto shadow_material = Material::Create(m_ShadowCasterShader);
					shadow_material->GetRasterState().CullMode = CullMode::Cull_None;
					return shadow_material;
				};

				/* 渲染每级联阴影，复用UpdateCascadeMatrices已计算好的VP矩阵与Layer */
				if (!m_CascadeShadowMaps.empty())
				{
					/* 一次性把全部级联 VP 矩阵写进光照 UBO（u_LightViewProjectionMat 数组），给阴影 Pass（按
					 * 级联索引取矩阵）和 LightingPass（按视距选级联）用；深度跨度和逐级联偏移也一起下发。 */
					std::vector<glm::mat4> light_vp_mats;
					std::vector<float> cascade_depth_spans;
					light_vp_mats.reserve(m_CascadeShadowMaps.size());
					cascade_depth_spans.reserve(m_CascadeShadowMaps.size());
					for (const auto& cascade_shadow_map : m_CascadeShadowMaps)
					{
						light_vp_mats.push_back(cascade_shadow_map->GetLightViewProjectionMat());
						cascade_depth_spans.push_back(cascade_shadow_map->GetDepthRange());
					}
					Renderer::FillLightUniformBuffer(m_CascadeShadowMaps[0]->m_pLight, light_vp_mats, m_CascadeSplits,
						cascade_depth_spans.empty() ? nullptr : &cascade_depth_spans);

					for (const auto& cascade_shadow_map : m_CascadeShadowMaps)
					{
						const uint8_t cascade_id = cascade_shadow_map->GetLayer();
						std::string label = "Cascade " + std::to_string(cascade_id);
						render_api->PushDebugGroup(label.c_str());

						auto shadow_material = make_caster_material();
						shadow_material->SetParameters(ParamType::Int, "u_CascadeIndex", static_cast<int>(cascade_id));

						render_shadow_layer(cascade_id, shadow_material);
						render_api->PopDebugGroup();
					}
				}

				/* 渲染点光 / 聚光阴影层：单面矩阵作为材质参数下发，u_CascadeIndex = -1 让 Shadow.glsl
				 * 走单矩阵分支。注意：参数名必须跟 Shadow.glsl 的裸 uniform 一致（u_ShadowPassMat），
				 * 也不能跟 LightUniformBuffer 成员重名（撞名编译不过）。 */
				for (const auto& punctual_shadow_map : m_PunctualShadowMaps)
				{
					const uint8_t layer = punctual_shadow_map->GetLayer();
					std::string label = "Punctual " + std::to_string(layer);
					render_api->PushDebugGroup(label.c_str());

					auto shadow_material = make_caster_material();
					shadow_material->SetParameters(ParamType::Int, "u_CascadeIndex", -1);
					shadow_material->SetParameters(ParamType::Mat4, "u_ShadowPassMat",
						punctual_shadow_map->GetLightViewProjectionMat());

					render_shadow_layer(layer, shadow_material);
					render_api->PopDebugGroup();
				}
			});

		/* 将阴影贴图句柄存储到Blackboard，供后续Pass使用 */
		frame_graph.GetBlackboard().Add("ShadowMapHandle", shadow_pass->GetData().ShadowMapHandle);
	}

}