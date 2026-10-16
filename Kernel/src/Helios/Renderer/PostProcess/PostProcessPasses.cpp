#include "Pch.h"
#include "PostProcessPasses.h"
#include "Effects/Bloom.h"
#include <Helios/Core/AssetManager.h>
#include <Helios/Renderer/FrameGraph/FrameGraph.h>
#include <Helios/Renderer/RenderView.h>
#include <Helios/Renderer/Renderer.h>
#include <Helios/Scene/Material.h>
#include <Helios/Scene/Mesh.h>
#include <Helios/Scene/SceneCommon.h>
#include <Helios/VirtualDevice/DeviceShader.h>

namespace Helios
{
	namespace PostProcess
	{
		bool HasEnabledEffects(const RenderView& render_view)
		{
			/* 调试视图不支持后处理 */
			if (render_view.GetDebugViewMode() != DebugViewMode::None)
				return false;

			const PostProcessState& state = render_view.GetPostProcessState();
			for (const EffectDesc& effect : PostProcessEffectRegistry::Instance().All())
			{
				if (effect.IsEnabled != nullptr && effect.IsEnabled(state))
					return true;
			}
			return false;
		}

		FrameGraphResourceHandleTyped<FrameGraphTexture> AddPostProcessPasses(
			RenderView& render_view, FrameGraphResourceHandleTyped<FrameGraphTexture> source)
		{
			PROFILE_FUNCTION();

			auto output = source;

			/* 调试视图时不执行后处理 */
			if (render_view.GetDebugViewMode() == DebugViewMode::None)
			{
				for (const EffectDesc& effect : PostProcessEffectRegistry::Instance().All())
				{
					if (effect.Build != nullptr)
						output = effect.Build(render_view, output);
				}
			}

			render_view.GetFrameGraph()->GetBlackboard()["PostProcessOutput"] = output;
			return output;
		}

		void AddPresentToDefaultTarget(
			RenderView& render_view, FrameGraphResourceHandleTyped<FrameGraphTexture> source)
		{
			PROFILE_FUNCTION();

			auto& frame_graph = render_view.GetFrameGraph();

			struct PresentPassData
			{
				FrameGraphResourceHandleTyped<FrameGraphTexture> Source;
			};

			frame_graph->AddPass<PresentPassData>("PostProcessPresent",
				[&](FrameGraphBuilder& builder, PresentPassData& data)
				{
					data.Source = source;
					builder.BindInputResource(data.Source, FrameGraphTexture::Usage::Sampleable);
					/* 无附件 = 渲染到默认目标；AsSideEffect 保活 */
					builder.AsSideEffect();
				},
				[&render_view](const FrameGraphResources& resources, const PresentPassData& data)
				{
					auto render_api = Renderer::GetRenderAPI();
					const auto& viewport_region = render_view.GetViewportRegion();
					render_api->SetViewport(0, 0, viewport_region.Width, viewport_region.Height);
					render_api->SetScissor(0, 0, viewport_region.Width, viewport_region.Height);

					const auto present_shader = AssetManager::Instance().GetOrLoad<DeviceShader>(
						ABSOLUTE_PATH("Shaders/PostProcessShaders/PostProcessPresent.glsl"));

					/* 全屏直出：深度写关（引擎约定 = 连深度测试一起关），不混合 */
					auto material = CreateSharedPtr<Material>();
					material->SetShader(present_shader);
					material->GetRasterState().EnableDepthWrite = false;
					material->SetTexture("u_Source", resources.Get(data.Source).Texture);
					Renderer::Submit(material, Renderer::GetFullScreenVertexArray());
					render_api->Flush();
				});
		}

		namespace
		{
			/* ==================== 内置效果注册清单 ====================
			 * 新增效果在这补一行。 */
			struct BuiltinPostProcessRegistration
			{
				BuiltinPostProcessRegistration()
				{
					RegisterBloomEffect();
				}
			};

			const BuiltinPostProcessRegistration s_BuiltinPostProcessRegistration;
		}
	}
}
