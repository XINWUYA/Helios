#include "Pch.h"
#include "GrassPasses.h"

#include <Helios/Core/AssetManager.h>
#include <Helios/Renderer/FrameGraph/FrameGraph.h>
#include <Helios/Renderer/RenderAPI.h>
#include <Helios/Renderer/Renderer.h>
#include <Helios/Renderer/RenderView.h>
#include <Helios/Scene/Components.h>
#include <Helios/Scene/GrassField.h>
#include <Helios/Scene/Scene.h>
#include <Helios/VirtualDevice/DeviceShader.h>

namespace Helios::GrassDraw
{
	/* 着色器在确认后端支持计算之后才用 static 声明：第一次走到那行才构造，Get 才真正加载 */
	void AddIndirectCullPass(RenderView& view)
	{
		if (!GrassField::IsGlobalIndirectDrawEnabled())
			return;

		/* 开关是意图，后端没有间接绘制就别挂这个 Pass：挂了也只是空跑一趟，
		 * 绘制端会退回批表，面板上的数字对不上 */
		if (Renderer::GetRenderAPI() == nullptr || !Renderer::GetRenderAPI()->SupportsIndirect())
			return;

		const SharedPtr<Scene> scene = view.GetOwnerScene();
		if (scene == nullptr || view.GetCullingCamera() == nullptr)
			return;

		/* 场景里没有可见的草就别占一个 Pass */
		bool has_grass = false;
		for (auto [entity, grass] : scene->GetRegistry().view<GrassComponent>().each())
		{
			if (grass.GetMaterial() == nullptr || !scene->IsEntityInView(entity, view.GetCullingMask()))
				continue;
			if (grass.m_Field == nullptr || grass.m_Field->IsEmpty())
				continue;
			has_grass = true;
			break;
		}
		if (!has_grass)
			return;

		struct CullPassData
		{
		};

		view.GetFrameGraph()->AddPass<CullPassData>("GrassIndirectCullPass",
			[](FrameGraphBuilder& builder, CullPassData&)
			{
				/* 结果写在 GrassField 自己的槽位缓冲里，图里暂时没有可绑的资源；
				 * 计算 Pass 不绑默认渲染目标 */
				builder.AsSideEffect();
				builder.AsComputePass();
			},
			[&view, scene](const FrameGraphResources&, const CullPassData&)
			{
				/* 级联矩阵在这之前已经定稿，各档视锥取得到 */
				scene->PrepareGrassIndirect(view);
			});
	}

	bool DispatchPlacement(const SharedPtr<DeviceStorageBuffer>& instances,
		const void* param_data, uint32_t param_size, uint32_t instance_count,
		const SharedPtr<DeviceStorageBuffer>& terrain_heights,
		const SharedPtr<DeviceStorageBuffer>& chunk_indices,
		const SharedPtr<DeviceStorageBuffer>& density_mask)
	{
		const SharedPtr<RenderAPI>& api = Renderer::GetRenderAPI();
		if (api == nullptr || instances == nullptr || instance_count == 0
			|| chunk_indices == nullptr || !api->SupportsCompute())
			return false;

		static BuiltinAsset<DeviceShader> s_placement_shader("Shaders/Grass/GrassPlacement.glsl");
		const SharedPtr<DeviceShader> shader = s_placement_shader.Get();
		if (shader == nullptr || !shader->IsCompute())
			return false;

		/* 1 号实例、2 号高度、3 号块表、4 号参数、5 号密度图；
		 * 高度和密度缺数据时仍要绑哑缓冲，避免 Metal 缺槽断言 */
		const SharedPtr<DeviceStorageBuffer> heights =
			(terrain_heights != nullptr) ? terrain_heights : DeviceStorageBuffer::Dummy();
		if (heights == nullptr)
			return false;

		const SharedPtr<DeviceStorageBuffer> mask = density_mask != nullptr
			? density_mask : DeviceStorageBuffer::Dummy();
		if (mask == nullptr)
			return false;

		const std::vector<std::pair<uint32_t, SharedPtr<DeviceStorageBuffer>>> buffers =
			{ { 1, instances }, { 2, heights }, { 3, chunk_indices }, { 5, mask } };
		const uint32_t group_size = std::max(1u, shader->GetThreadGroupSize());
		const uint32_t group_count = (instance_count + group_size - 1) / group_size;
		return api->DispatchCompute(shader, buffers, 4, param_data, param_size, group_count);
	}

	bool DispatchArgsReset(const SharedPtr<DeviceStorageBuffer>& args,
		const SharedPtr<DeviceStorageBuffer>& chunk_list,
		uint32_t index_count, uint32_t segments, uint32_t args_stride)
	{
		const SharedPtr<RenderAPI>& api = Renderer::GetRenderAPI();
		if (api == nullptr || args == nullptr || chunk_list == nullptr || !api->SupportsCompute())
			return false;

		static BuiltinAsset<DeviceShader> s_args_reset_shader("Shaders/Grass/GrassIndirectReset.glsl");
		const SharedPtr<DeviceShader> shader = s_args_reset_shader.Get();
		if (shader == nullptr || !shader->IsCompute())
			return false;

		const glm::uvec4 reset(index_count, segments, args_stride, 0u);
		const std::vector<std::pair<uint32_t, SharedPtr<DeviceStorageBuffer>>> buffers = {
			{ 3, args }, { 6, chunk_list },
		};
		return api->DispatchCompute(shader, buffers, 4, &reset, sizeof(reset), 1);
	}

	bool DispatchChunkCull(const SharedPtr<DeviceStorageBuffer>& chunk_bounds,
		const SharedPtr<DeviceStorageBuffer>& chunk_list, const SharedPtr<DeviceStorageBuffer>& args,
		const void* param_data, uint32_t param_size, uint32_t thread_count)
	{
		const SharedPtr<RenderAPI>& api = Renderer::GetRenderAPI();
		if (api == nullptr || chunk_bounds == nullptr || chunk_list == nullptr
			|| args == nullptr || thread_count == 0 || !api->SupportsCompute())
			return false;

		static BuiltinAsset<DeviceShader> s_chunk_cull_shader("Shaders/Grass/GrassChunkCull.glsl");
		const SharedPtr<DeviceShader> shader = s_chunk_cull_shader.Get();
		if (shader == nullptr || !shader->IsCompute())
			return false;

		/* 5 号块包围球、6 号可见块表，参数块在 4 号 */
		const std::vector<std::pair<uint32_t, SharedPtr<DeviceStorageBuffer>>> buffers = {
			{ 3, args }, { 5, chunk_bounds }, { 6, chunk_list },
		};
		const uint32_t group_size = std::max(1u, shader->GetThreadGroupSize());
		const uint32_t group_count = (thread_count + group_size - 1) / group_size;
		return api->DispatchCompute(shader, buffers, 4, param_data, param_size, group_count);
	}

	bool DispatchCull(const SharedPtr<DeviceStorageBuffer>& source,
		const SharedPtr<DeviceStorageBuffer>& target, const SharedPtr<DeviceStorageBuffer>& args,
		const SharedPtr<DeviceStorageBuffer>& chunk_list,
		const void* param_data, uint32_t param_size, uint32_t fallback_instance_count,
		const SharedPtr<DeviceStorageBuffer>& dispatch_args, uint32_t dispatch_offset)
	{
		const SharedPtr<RenderAPI>& api = Renderer::GetRenderAPI();
		if (api == nullptr || source == nullptr || target == nullptr || args == nullptr
			|| chunk_list == nullptr || !api->SupportsCompute())
			return false;

		/* 同一份着色器的块表变体，见文件里的 @variant */
		static BuiltinAsset<DeviceShader> s_cull_shader("Shaders/Grass/GrassIndirectCull.glsl",
			ShaderVariantConfig{ ShaderVariantDefines{ { "GRASS_CULL_CHUNKED", 1 } } });
		const SharedPtr<DeviceShader> shader = s_cull_shader.Get();
		if (shader == nullptr || !shader->IsCompute())
			return false;

		/* 1 源、2 压实目标、3 间接参数、5 可见块表，参数块在 4 号 */
		const std::vector<std::pair<uint32_t, SharedPtr<DeviceStorageBuffer>>> buffers = {
			{ 1, source }, { 2, target }, { 3, args }, { 5, chunk_list },
		};

		/* 先按可见块数间接派发；后端没有间接派发就退回按上界派发 */
		if (dispatch_args != nullptr
			&& api->DispatchComputeIndirect(shader, buffers, 4, param_data, param_size,
				dispatch_args, dispatch_offset))
			return true;

		if (fallback_instance_count == 0)
			return false;
		const uint32_t group_size = std::max(1u, shader->GetThreadGroupSize());
		const uint32_t group_count = (fallback_instance_count + group_size - 1) / group_size;
		return api->DispatchCompute(shader, buffers, 4, param_data, param_size, group_count);
	}

	bool DispatchCullFull(const SharedPtr<DeviceStorageBuffer>& source,
		const SharedPtr<DeviceStorageBuffer>& target, const SharedPtr<DeviceStorageBuffer>& args,
		const SharedPtr<DeviceStorageBuffer>& chunk_bounds,
		const void* param_data, uint32_t param_size, uint32_t instance_count)
	{
		const SharedPtr<RenderAPI>& api = Renderer::GetRenderAPI();
		if (api == nullptr || source == nullptr || target == nullptr || args == nullptr
			|| chunk_bounds == nullptr || instance_count == 0 || !api->SupportsCompute())
			return false;

		/* 同一份着色器的逐实例变体：不做块级预剔，自己测视锥 */
		static BuiltinAsset<DeviceShader> s_cull_full_shader("Shaders/Grass/GrassIndirectCull.glsl",
			ShaderVariantConfig{ ShaderVariantDefines{ { "GRASS_CULL_CHUNKED", 0 } } });
		const SharedPtr<DeviceShader> shader = s_cull_full_shader.Get();
		if (shader == nullptr || !shader->IsCompute())
			return false;

		/* 1 源、2 压实目标、3 间接参数、5 块包围球，参数块在 4 号 */
		const std::vector<std::pair<uint32_t, SharedPtr<DeviceStorageBuffer>>> buffers = {
			{ 1, source }, { 2, target }, { 3, args }, { 5, chunk_bounds },
		};
		const uint32_t group_size = std::max(1u, shader->GetThreadGroupSize());
		const uint32_t group_count = (instance_count + group_size - 1) / group_size;
		return api->DispatchCompute(shader, buffers, 4, param_data, param_size, group_count);
	}
}
