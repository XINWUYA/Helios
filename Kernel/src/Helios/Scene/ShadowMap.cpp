#include "Pch.h"
#include "ShadowMap.h"
#include "Scene.h"
#include <cmath>
#include <glm/gtc/type_ptr.hpp>
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

				/* todo */
				break;
			case LightType::Spot:
				/* todo: */
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

		/* 准备方向光的ShadowMap：计算级联 VP 矩阵与分割距离（逻辑收口于 UpdateCascadeMatrices） */
		UpdateCascadeMatrices(camera);
	}

	/* 依据当前方向光方向与相机，重算各级联的视图投影矩阵与分割距离。
	 * 供 PrepareForShadowMaps（首次准备）与每帧执行前（方向光旋转 / 相机移动）共用。
	 * 通过对比缓存（方向光方向 + 相机视图/投影矩阵）跳过无变化的重算。 */
	void ShadowMapManager::UpdateCascadeMatrices(const Camera* camera)
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
		const bool unchanged =
			!m_IsDirty &&
			dir_equal &&
			mat_equal(view_mat, m_LastViewMat) &&
			mat_equal(proj_mat, m_LastProjMat);
		if (unchanged)
			return;

		/* 记录本次用于计算的输入，供下次对比 */
		m_LastLightDir = direction;
		m_LastViewMat = view_mat;
		m_LastProjMat = proj_mat;
		m_IsDirty = false;


		const uint8_t cascade_cnt = shadow_map_info->CascadeCnt;
		ASSERT(cascade_cnt <= SHADOW_CASCADE_MAX_NUM);

		// 计算光照视图矩阵（所有级联共享）
		auto light_view_mat = ShadowMap::GetDirectionalLightViewMatrix(direction);

		/* CSM 子视锥分割：按相机视距把主视锥切成 cascade_cnt 段，每段对应一个级联。用对数分割与
		 * 均匀分割的混合（λ 混合），近处精度和远处稳定性兼顾。split[i] = 第 i 段远边界在视图空间的距离。 */
		const float lambda = 0.3f;// 0.5f; // 0 = 均匀，1 = 纯对数
		const float cam_near = camera ? camera->GetNearClip() : 0.1f;
		const float cam_far  = camera ? camera->GetFarClip()  : shadow_map_info->ShadowFar;

		std::vector<float> split_dist(cascade_cnt + 1);
		split_dist[0] = cam_near;
		for (uint8_t i = 1; i <= cascade_cnt; ++i)
		{
			const float f = static_cast<float>(i) / static_cast<float>(cascade_cnt);
			const float log_split = cam_near * std::pow(cam_far / cam_near, f);
			const float uni_split = cam_near + (cam_far - cam_near) * f;
			split_dist[i] = lambda * log_split + (1.0f - lambda) * uni_split;
		}

		// 相机世界矩阵（用于把视图空间子视锥角点变换回世界空间）；view_mat 已在脏检测处获取
		const glm::mat4 cam_world_mat = glm::inverse(view_mat);

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
			const float radius = shadow_map_info->CascadeRadius[cascade_id];

			float ortho_left   = ls_min.x;
			float ortho_right  = ls_max.x;
			float ortho_bottom = ls_min.y;
			float ortho_top    = ls_max.y;

			// 居中并取对称正方形范围（以较长边为准），提升 PCF 采样稳定性
			const float center_x = (ortho_left + ortho_right) * 0.5f;
			const float center_y = (ortho_bottom + ortho_top) * 0.5f;
			const float half_x = (ortho_right - ortho_left) * 0.5f;
			const float half_y = (ortho_top - ortho_bottom) * 0.5f;
			float half_extent = std::max(half_x, half_y);
			// 下限：保证范围不小于级联半径（仅放大、不缩小真实 AABB）
			half_extent = std::max(half_extent, radius);

			ortho_left   = center_x - half_extent;
			ortho_right  = center_x + half_extent;
			ortho_bottom = center_y - half_extent;
			ortho_top    = center_y + half_extent;

			// 将范围对齐到阴影纹素尺寸，避免相机移动时级联阴影抖动（shimmering）
			const float texel_size = (2.0f * half_extent) / static_cast<float>(shadow_map_info->Size);
			ortho_left   = std::floor(ortho_left   / texel_size) * texel_size;
			ortho_right  = std::ceil (ortho_right  / texel_size) * texel_size;
			ortho_bottom = std::floor(ortho_bottom / texel_size) * texel_size;
			ortho_top    = std::ceil (ortho_top    / texel_size) * texel_size;

			// near/far 由光照空间 Z 范围推导（视图空间 Z = -光照空间 Z）
			const float epsilon = 0.05f;
			float ortho_near = -ls_max.z - epsilon; // 最近点（光照空间 Z 最大）
			float ortho_far  = -ls_min.z + epsilon; // 最远点（光照空间 Z 最小）
			if (ortho_far <= ortho_near) ortho_far = ortho_near + 0.1f;

			glm::mat4 light_proj_mat = glm::ortho(ortho_left, ortho_right, ortho_bottom, ortho_top, ortho_near, ortho_far);

			// 计算并设置光照视图投影矩阵
			glm::mat4 light_view_proj_mat = light_proj_mat * light_view_mat;
			m_CascadeShadowMaps[cascade_id]->SetLightViewProjectionMat(light_view_proj_mat);

			// 记录该级联远边界（视图空间距离），供着色阶段选择级联
			cascade_splits[cascade_id] = seg_far;
		}

		// 保存级联分割距离，供 AddShadowPass 填充光照 UBO
		m_CascadeSplits = cascade_splits;
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

		/* 点光/聚光 */
		for (auto& shadow_map : m_PunctualShadowMaps)
		{
			shadow_map->SetLayer(layer++);
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

	/* 将ShadowPass注入到FrameGraph */
	void ShadowMapManager::AddShadowPass(FrameGraph& frame_graph, const SharedPtr<Scene>& scene, RenderView* render_view)
	{
		/* 若尚未准备（如FrameGraph setup阶段早于RenderView每帧的PrepareLights），
		 * 则先基于当前Scene准备阴影纹理描述，确保setup阶段能创建正确尺寸的纹理资源
		 */
		if (m_CascadeShadowMaps.empty())
			PrepareForShadowMaps(scene, render_view ? render_view->GetCullingCamera() : nullptr);

		if (m_CascadeShadowMaps.empty())
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

				/* 一次性将全部级联的VP矩阵写入光照UBO（u_LightViewProjectionMat 数组），
				 * 供阴影Pass（按级联索引取对应矩阵）与后续LightingPass（按视距选级联）共同使用。 */
				std::vector<glm::mat4> light_vp_mats;
				light_vp_mats.reserve(m_CascadeShadowMaps.size());
				for (const auto& cascade_shadow_map : m_CascadeShadowMaps)
					light_vp_mats.push_back(cascade_shadow_map->GetLightViewProjectionMat());
				Renderer::FillLightUniformBuffer(m_CascadeShadowMaps[0]->m_pLight, light_vp_mats, m_CascadeSplits);

				auto render_api = Renderer::GetRenderAPI();

				/* 渲染每级联阴影，复用PrepareAllShadowMaps已计算好的VP矩阵与Layer */
				for (const auto& cascade_shadow_map : m_CascadeShadowMaps)
				{
					const uint8_t cascade_id = cascade_shadow_map->GetLayer();
					std::string label = "Cascade " + std::to_string(cascade_id);
					render_api->PushDebugGroup(label.c_str());

					/* 绑定FrameBuffer为指定cascade_id层（统一 Bind 接口，自动设置 viewport 并按层附着 Depth 附件） */
					render_pass_info->Bind(FrameBufferBindInfo::ToDepthLayer(cascade_id));

					render_api->SetViewport(0, 0, m_RequiredTextureDesc.Size, m_RequiredTextureDesc.Size);
					render_api->SetScissor(0, 0, m_RequiredTextureDesc.Size, m_RequiredTextureDesc.Size);
					render_api->Clear();

					/* 设置阴影Shader的级联索引 */
					auto shadow_material = Material::Create(m_ShadowCasterShader);
					shadow_material->SetParameters(ParamType::Int, "u_CascadeIndex", static_cast<int>(cascade_id));

					/* 渲染场景到阴影贴图 */
					for (const auto& mesh_object : render_view->GetVisibleMeshObjects())
					{
						Renderer::FillObjectUniformBuffer(mesh_object);
						Renderer::Submit(shadow_material, mesh_object.MeshSegment->GetMeshPrimitive());
					}

					render_pass_info->Unbind();
					render_api->PopDebugGroup();
				}
			});

		/* 将阴影贴图句柄存储到Blackboard，供后续Pass使用 */
		frame_graph.GetBlackboard().Add("ShadowMapHandle", shadow_pass->GetData().ShadowMapHandle);
	}

}