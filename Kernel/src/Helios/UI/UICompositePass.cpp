#include "Pch.h"
#include "UICompositePass.h"

#include "UISystem.h"

#include "Helios/Core/AssetManager.h"
#include "Helios/Renderer/FrameGraph/FrameGraph.h"
#include "Helios/Renderer/RenderAPI.h"
#include "Helios/Renderer/RenderView.h"
#include "Helios/Renderer/Renderer.h"
#include "Helios/Scene/Material.h"
#include "Helios/Scene/Scene.h"
#include "Helios/VirtualDevice/DeviceShader.h"
#include "Helios/VirtualDevice/DeviceTexture.h"

namespace Helios
{
	namespace UI
	{
		FrameGraphResourceHandleTyped<FrameGraphTexture> AddCompositePass(RenderView& view,
			FrameGraphResourceHandleTyped<FrameGraphTexture> source)
		{
			if (!view.IsDrawUI())
				return source;

			const SharedPtr<Scene> scene = view.GetOwnerScene();
			if (!scene || !scene->HasUICanvas())
				return source;

			const ViewportRegion& region = view.GetViewportRegion();
			if (region.Width == 0 || region.Height == 0)
				return source;

			UISystem& ui_system = scene->GetUISystem();
			auto& frame_graph = view.GetFrameGraph();

			struct CompositePassData
			{
				FrameGraphResourceHandleTyped<FrameGraphTexture> Source;
				FrameGraphResourceHandleTyped<FrameGraphTexture> Output;
			};

			FrameGraphTexture::Descriptor color_desc;
			color_desc.Width = region.Width;
			color_desc.Height = region.Height;
			color_desc.TextureFormat = TextureFormat::RGBA8;

			auto pass = frame_graph->AddPass<CompositePassData>("UIComposite",
				[&](FrameGraphBuilder& builder, CompositePassData& data)
				{
					data.Source = source;
					data.Output = builder.CreateTexture("ViewOutput", color_desc);
					builder.BindInputResource(data.Source, FrameGraphTexture::Usage::Sampleable);
					builder.BindOutputResource(data.Output,
						FrameGraphTexture::Usage::ColorAttachment | FrameGraphTexture::Usage::Sampleable);

					FrameGraphPassInfo::Descriptor pass_desc;
					pass_desc.Attachments.ColorAttachments(0) = data.Output;
					pass_desc.ViewportRegion = region;
					builder.CreateRenderPass("UICompositeRenderTarget", pass_desc);

					/* 输出要被界面采样，既不能因无人消费被剔除，也不能在帧末随资源释放 */
					builder.AsSideEffect();
				},
				[&view, &ui_system](const FrameGraphResources& resources, const CompositePassData& data)
				{
					auto render_api = Renderer::GetRenderAPI();
					if (render_api == nullptr)
						return;

					const auto render_pass_info = resources.GetPassRenderTarget();
					render_pass_info->Bind();
					{
						/* 整屏拷贝管线输出：UI 要叠在最终画面之上，之后再接编辑器叠加层 */
						const ViewportRegion& viewport = view.GetViewportRegion();
						render_api->SetViewport(0, 0, viewport.Width, viewport.Height);
						render_api->SetScissor(0, 0, viewport.Width, viewport.Height);

						static BuiltinAsset<DeviceShader> s_present_shader(
							"Shaders/PostProcessShaders/PostProcessPresent.glsl");
						auto material = Renderer::CreateFullScreenMaterial(s_present_shader.Get());
						material->SetTexture("u_Source", resources.Get(data.Source).Texture);
						Renderer::Submit(material, Renderer::GetFullScreenVertexArray());
						render_api->Flush();

						ui_system.Render(view);
					}
					render_pass_info->Unbind();
				});

			const auto output = pass->GetData().Output;
			frame_graph->GetBlackboard()["ViewOutput"] = output;
			return output;
		}
	}
}
