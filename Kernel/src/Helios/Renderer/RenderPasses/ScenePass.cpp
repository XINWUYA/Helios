#include "Pch.h"
#include "ScenePass.h"
#include <Helios/Renderer/FrameGraph/FrameGraph.h>
#include <Helios/Renderer/RenderView.h>
#include <Helios/Renderer/Renderer.h>
#include <Helios/Scene/Mesh.h>
#include <Helios/Scene/Material.h>
#include <Helios/Scene/ReflectionProbe.h>
#include <Helios/Scene/Scene.h>
#include <Helios/VirtualDevice/DeviceFrameBuffer.h>
#include <Helios/VirtualDevice/DeviceShader.h>

namespace Helios
{
	namespace Forward
	{
		namespace
		{
			/* 前向绘制的公共体：可见对象逐一提交（含按对象选最近探针的 IBL per-draw 覆盖）。
			 * 调用方负责绑定好目标、设置视口并挂好阴影纹理。 */
			void SubmitForwardObjects(RenderView& render_view, const SharedPtr<Scene>& scene)
			{
				const auto probe_manager = scene->GetReflectionProbeManager();
				for (const auto& mesh_object : render_view.GetVisibleMeshObjects())
				{
					Renderer::FillObjectUniformBuffer(mesh_object);
					const auto& material = mesh_object.Material;
					if (material == nullptr)
						continue;

					/* IBL 是"每次绘制都不一样"的绑定（按对象选最近探针）—— 走 per-draw 覆盖、不写进材质对象；
					 * 只对支持 IBL 的 Shader 发；没有探针时靠覆盖里的中性兜底，保证四个采样槽始终有绑定。
					 * 覆盖随每个光照笔重发（声明过的采样器每次绘制都要有绑定）。 */
					DrawParams per_draw;
					if (material->SupportsIBL())
					{
						const glm::vec3 world_pos = glm::vec3(mesh_object.Local2WorldMat[3]);
						const auto chosen = (probe_manager && probe_manager->HasProbe())
							? probe_manager->GetClostedReflectionProbe(world_pos)
							: nullptr;
						const SharedPtr<DeviceTexture> brdf_lut = (probe_manager != nullptr)
							? probe_manager->GetBRDFLutMap() : nullptr;

						Material::MakeIBLParamOverrides(per_draw.Overrides,
							chosen != nullptr ? chosen->GetIrradianceMap() : nullptr,
							chosen != nullptr ? chosen->GetPrefilterMap() : nullptr,
							brdf_lut);
					}
					Renderer::Submit(material, mesh_object.MeshSegment->GetMeshPrimitive(),
						per_draw.Overrides.empty() ? nullptr : &per_draw);
				}
			}
		}

		struct ScenePassData
		{
			FrameGraphResourceHandleTyped<FrameGraphTexture> ShadowMapHandle; /* 阴影贴图 */
		};

		void AddScenePass(RenderView* render_view)
		{
			if (!render_view) return;

			auto& frame_graph = render_view->GetFrameGraph();
			if (!frame_graph) return;

            auto scene = render_view->GetOwnerScene();
            if (!scene) return;

			frame_graph->AddPass<ScenePassData>("ScenePass",
				[&](FrameGraphBuilder& builder, ScenePassData& data)
				{
					data.ShadowMapHandle = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("ShadowMapHandle");
					builder.BindInputResource(data.ShadowMapHandle, FrameGraphTexture::Usage::Sampleable);
					builder.AsSideEffect();
				},
				[&, render_view, scene](const FrameGraphResources& resources, const ScenePassData& data)
				{
					auto render_api = Renderer::GetRenderAPI();
					ScopedShadowMapBinding shadow_map_binding(resources.Get(data.ShadowMapHandle).Texture);
					{
						render_api->Clear();
						auto& viewport_region = render_view->GetViewportRegion();
						render_api->SetViewport(0, 0, viewport_region.Width, viewport_region.Height);
						render_api->SetScissor(0, 0, viewport_region.Width, viewport_region.Height);

						SubmitForwardObjects(*render_view, scene);
					}
				}
			);
		}

		struct ScenePassTextureData
		{
			FrameGraphResourceHandleTyped<FrameGraphTexture> ShadowMapHandle;
			FrameGraphResourceHandleTyped<FrameGraphTexture> Output;   /* rgb: 最终颜色 */
			FrameGraphResourceHandleTyped<FrameGraphTexture> ObjectId; /* 实体 ID（清 -1，供按像素拾取） */
			FrameGraphResourceHandleTyped<FrameGraphTexture> Depth;    /* 供叠加层做深度比较 */
		};

		void AddScenePassToTexture(RenderView* render_view)
		{
			if (!render_view) return;

			auto& frame_graph = render_view->GetFrameGraph();
			if (!frame_graph) return;

            auto scene = render_view->GetOwnerScene();
            if (!scene) return;

			const auto& viewport_region = render_view->GetViewportRegion();

			FrameGraphTexture::Descriptor color_desc;
			color_desc.Width = viewport_region.Width;
			color_desc.Height = viewport_region.Height;
			color_desc.TextureFormat = TextureFormat::RGBA8;

			FrameGraphTexture::Descriptor object_id_desc;
			object_id_desc.Width = viewport_region.Width;
			object_id_desc.Height = viewport_region.Height;
			object_id_desc.TextureFormat = TextureFormat::R32I;

			FrameGraphTexture::Descriptor depth_desc;
			depth_desc.Width = viewport_region.Width;
			depth_desc.Height = viewport_region.Height;
			depth_desc.TextureFormat = TextureFormat::Depth32;

			auto scene_pass = frame_graph->AddPass<ScenePassTextureData>("ScenePass",
				[&](FrameGraphBuilder& builder, ScenePassTextureData& data)
				{
					data.ShadowMapHandle = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("ShadowMapHandle");
					data.Output = builder.CreateTexture("ScenePassOutput", color_desc);
					data.ObjectId = builder.CreateTexture("ScenePassObjectId", object_id_desc);
					data.Depth = builder.CreateTexture("ScenePassDepth", depth_desc);

					builder.BindInputResource(data.ShadowMapHandle, FrameGraphTexture::Usage::Sampleable);
					builder.BindOutputResource(data.Output, FrameGraphTexture::Usage::ColorAttachment | FrameGraphTexture::Usage::Sampleable);
					builder.BindOutputResource(data.ObjectId, FrameGraphTexture::Usage::ColorAttachment | FrameGraphTexture::Usage::Sampleable);
					builder.BindOutputResource(data.Depth, FrameGraphTexture::Usage::DepthAttachment);

					FrameGraphPassInfo::Descriptor pass_desc;
					pass_desc.Attachments.ColorAttachments(0) = data.Output;
					pass_desc.Attachments.ColorAttachments(1) = data.ObjectId;
					pass_desc.Attachments.DepthAttachment() = data.Depth;
					/* ObjectId 清成 -1（空像素不可拾取；与延迟 GBuffer 同款约定） */
					pass_desc.ColorClearValues.resize(2);
					pass_desc.ColorClearValues[1] = glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f);
					pass_desc.ViewportRegion = viewport_region;
					builder.CreateRenderPass("ScenePassRenderTarget", pass_desc);

					/* 输出纹理留给视图显示与拾取回读：不能被图裁剪剔除 */
					builder.AsSideEffect();
				},
				[render_view, scene](const FrameGraphResources& resources, const ScenePassTextureData& data)
				{
					const auto render_pass_info = resources.GetPassRenderTarget();
					render_view->EmplacePassFrameBuffer("ScenePass", render_pass_info);

					render_pass_info->Bind();
					{
						ScopedShadowMapBinding shadow_map_binding(resources.Get(data.ShadowMapHandle).Texture);
						Renderer::GetRenderAPI()->Clear();

						SubmitForwardObjects(*render_view, scene);
					}
					render_pass_info->Unbind();
					Renderer::GetRenderAPI()->Flush();
				}
				);

			frame_graph->GetBlackboard()["ScenePassOutput"] = scene_pass->GetData().Output;
			frame_graph->GetBlackboard()["ScenePassDepth"] = scene_pass->GetData().Depth;
			render_view->SetRenderTargetHandle(scene_pass->GetData().Output);
		}
	}
}
