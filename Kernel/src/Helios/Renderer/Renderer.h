#pragma once
#include <utility>
#include "RenderAPI.h"
#include "RenderQuery.h"

namespace Helios
{
	class RenderView;
	class Material;
	class DeviceTexture;
	class DeviceUniformBuffer;
	class ScopedShadowMapBinding;
	class ShadowMapManager;
	struct VisibleMeshObject;
	class Light;
	struct MeshPrimitive;

	class Renderer
	{
	public:
		static void Init();
		static void Update();
		static void Release();

		static void SetViewport(uint32_t x_start, uint32_t y_start, uint32_t width, uint32_t height);
		static void SetClearColor(const glm::vec4& color);
		static void Clear();

		/* 绘制一个视图 */
		static void RenderAView(RenderView* view);

		/* 指定ViewUniformBuffer */
		static void SetViewUniforms(const glm::mat4& view, const glm::mat4& projection, const glm::vec3& view_pos);

		static void Submit(const SharedPtr<Material>& material, const MeshPrimitive& mesh_primitive, uint32_t index_count = 0);

		static int CurrentAPI() { return RenderAPI::GetAPI(); }

		static const SharedPtr<RenderAPI>& GetRenderAPI() { return m_pRenderAPI; }

		static const ResultGPUTimerNode& GetGPUTimerRoot() { return m_GPUTimerRoot; }

		static SharedPtr<DeviceVertexArray> GetFullScreenVertexArray();

		static void FillObjectUniformBuffer(const VisibleMeshObject& mesh_object);

		/* 填充光照UniformBuffer；light_vp_mats 为级联阴影的光照视图投影矩阵数组，
		 * cascade_splits 为各级联在视图空间的分割距离（远边界）。
		 * 供阴影 Pass 在渲染级联前调用（Shadow.glsl 按 u_CascadeIndex 取矩阵）。 */
		static void FillLightUniformBuffer(const SharedPtr<Light>& light, const std::vector<glm::mat4>& light_vp_mats, const glm::vec4& cascade_splits);
		/* 光照阶段入口：填充完整数据 —— 光源参数 + 该光源的阴影数据
		 * （方向光取级联数组，点光/聚光取面矩阵与层信息，均来自本视图的阴影管理器）。
		 * shadow_maps 为空表示本光源不参与阴影（字段按"无阴影"填充）。 */
		static void FillLightUniformBuffer(const SharedPtr<Light>& light, const ShadowMapManager* shadow_maps);

	private:
		static SharedPtr<DeviceTexture> ExchangeCurrentShadowMap(SharedPtr<DeviceTexture> shadow_map);
		friend class ScopedShadowMapBinding;
		static SharedPtr<RenderAPI> m_pRenderAPI;
		static ResultGPUTimerNode m_GPUTimerRoot;
		static SharedPtr<DeviceTexture> m_pCurrentShadowMap;
	};

	/* 设置当前 Pass 的阴影纹理，并在作用域结束时恢复前值。 */
	class ScopedShadowMapBinding final
	{
	public:
		explicit ScopedShadowMapBinding(SharedPtr<DeviceTexture> shadow_map)
			: m_Previous(Renderer::ExchangeCurrentShadowMap(std::move(shadow_map)))
		{
		}

		~ScopedShadowMapBinding()
		{
			Renderer::ExchangeCurrentShadowMap(std::move(m_Previous));
		}

		ScopedShadowMapBinding(const ScopedShadowMapBinding&) = delete;
		ScopedShadowMapBinding& operator=(const ScopedShadowMapBinding&) = delete;

	private:
		SharedPtr<DeviceTexture> m_Previous;
	};
}
