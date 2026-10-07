#include "Pch.h"
#include "ScenePass.h"
#include <Helios/Renderer/FrameGraph/FrameGraph.h>
#include <Helios/Renderer/RenderView.h>
#include <Helios/Renderer/Renderer.h>
#include <Helios/Scene/Mesh.h>
#include <Helios/Scene/Material.h>
#include <Helios/Scene/ReflectionProbe.h>
#include <Helios/Scene/Scene.h>

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
							if (probe_manager && probe_manager->HasProbe())
							{
								/* 选择最近且已烘焙的探针 */
								const glm::vec3 world_pos = glm::vec3(mesh_object.Local2WorldMat[3]);
								const auto chosen = probe_manager->GetClostedReflectionProbe(world_pos);
								if (chosen)
								{
									/* 指定IBL资源 */
									material->SetParameters(ParamType::Int, "u_UseIBL", 1);
									material->SetTexture("u_BRDFLut", probe_manager->GetBRDFLutMap());
									material->SetTexture("u_IrradianceMap", chosen->GetIrradianceMap());
									material->SetTexture("u_PrefilterMap", chosen->GetPrefilterMap());
								}
							}
							Renderer::Submit(material, mesh_object.MeshSegment->GetMeshPrimitive());
						}
					}
				}
			);
		}

	}
}