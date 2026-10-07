#include "Pch.h"
#include "ScenePass.h"
#include <Helios/Renderer/FrameGraph/FrameGraph.h>
#include <Helios/Renderer/RenderView.h>
#include <Helios/Renderer/Renderer.h>
#include <Helios/Scene/Mesh.h>
#include <Helios/Scene/Material.h>
#include <Helios/Scene/ReflectionProbe.h>
#include <Helios/Scene/Scene.h>
#include <Helios/VirtualDevice/DeviceShader.h>

namespace Helios
{
	namespace Forward
	{
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

						const auto probe_manager = scene->GetReflectionProbeManager();
						for (const auto& mesh_object : render_view->GetVisibleMeshObjects())
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
			);
		}

	}
}