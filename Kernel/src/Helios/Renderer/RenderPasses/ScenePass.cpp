#include "Pch.h"
#include "ScenePass.h"
#include "DebugViewPasses.h"
#include "ObjectSorting.h"
#include <Helios/Renderer/FrameGraph/FrameGraph.h>
#include <Helios/Renderer/PostProcess/PostProcessPasses.h>
#include <Helios/Renderer/RenderView.h>
#include <Helios/Renderer/Renderer.h>
#include <Helios/Scene/Camera.h>
#include <Helios/Scene/Light.h>
#include <Helios/Scene/Mesh.h>
#include <Helios/Scene/Material.h>
#include <Helios/Scene/ReflectionProbe.h>
#include <Helios/Scene/Scene.h>
#include <Helios/Scene/ShadowMap.h>
#include <Helios/VirtualDevice/DeviceFrameBuffer.h>
#include <Helios/VirtualDevice/DeviceShader.h>

namespace Helios
{
	namespace Forward
	{
		namespace
		{
			/* 逐光源加法笔的光栅状态：在材质状态上只改混合和深度比较 —— 加法混合（One / One / Add）、
			 * 深度比较 Equal（只在第 0 笔画过的像素上叠加）。深度写沿用本批的基础状态：
			 * 不透明批写着、透明批不写。 */
			RenderRasterState MakeAdditiveRasterState(const RenderRasterState& base)
			{
				RenderRasterState state = base;
				state.EnableBlend = true;
				state.BlendEquationRGB = BlendEquation::Add;
				state.BlendEquationA = BlendEquation::Add;
				state.BlendFuncSrcRGB = BlendFunc::One;
				state.BlendFuncSrcA = BlendFunc::One;
				state.BlendFuncDstRGB = BlendFunc::One;
				state.BlendFuncDstA = BlendFunc::One;
				state.DepthCompareFunc = CompareFunc::Equal;
				return state;
			}

			/* 前向绘制公共体：按渲染队列分两批提交（不透明先、透明后），每批内逐光源一笔合成 ——
			 * 第 0 笔完整着色（直接光 + 环境 / 自发光，用材质自身的光栅状态），其余笔加法混合 +
			 * Equal 比较，只输出该光源的直接光。天空盒和 Unlit 单笔原样提交；
			 * 调用方自己负责绑目标、视口和阴影纹理。 */
			void SubmitForwardObjects(RenderView& render_view, Scene& scene, bool transparent)
			{
				const auto probe_manager = scene.GetReflectionProbeManager();
				const auto& lights = render_view.GetValidLights();
				const ShadowMapManager* shadow_maps = render_view.GetShadowMapManager().get();
				const glm::vec3 view_position = (render_view.GetCullingCamera() != nullptr)
					? render_view.GetCullingCamera()->GetPosition() : glm::vec3(0.0f);

				const auto draw_list = RenderSorting::CollectDrawList(
					render_view.GetVisibleMeshObjects(), transparent, view_position);

				/* 环境项放在主阴影光源那一笔里合成：只有那笔的阴影因子非 0，
				 * 环境项才吃得到阴影（见 AmbientShadowVisibility） */
				const size_t ambient_light_index = (shadow_maps != nullptr)
					? shadow_maps->FindPrimaryShadowLightIndex(lights) : 0;

				for (const VisibleMeshObject* mesh_object : draw_list)
				{
					Renderer::FillObjectUniformBuffer(*mesh_object);
					const auto& material = mesh_object->Material;
					const RenderRasterState base_state = transparent
						? RenderSorting::MakeTransparentRasterState(material->GetRasterState())
						: material->GetRasterState();

					/* IBL 是"每次绘制都不一样"的绑定（按对象选最近探针）—— 走 per-draw 覆盖、不写进材质对象；
					 * 只对支持 IBL 的 Shader 发；没有探针时靠覆盖里的中性兜底，保证四个采样槽始终有绑定。
					 * 覆盖随每个光照笔重发（声明过的采样器每次绘制都要有绑定）。 */
					DrawParams per_draw;
					if (transparent)
						per_draw.RasterStateOverride = base_state;

					/* 前向直画用材质自身 shader，不经过 G-Buffer 的采样器兜底：材质没写的贴图槽
					 * 要用默认贴图补上，缺一个声明过的采样器 Metal 就会直接断言。 */
					per_draw.Overrides = material->CollectMissingTextureParamDefaults();

					if (material->SupportsIBL())
					{
						const glm::vec3 world_pos = glm::vec3(mesh_object->Local2WorldMat[3]);
						const auto chosen = (probe_manager && probe_manager->HasProbe())
							? probe_manager->GetClostedReflectionProbe(world_pos, render_view.GetCullingMask())
							: nullptr;
						const SharedPtr<DeviceTexture> brdf_lut = (probe_manager != nullptr)
							? probe_manager->GetBRDFLutMap() : nullptr;

						Material::MakeIBLParamOverrides(per_draw.Overrides,
							chosen != nullptr ? chosen->GetIrradianceMap() : nullptr,
							chosen != nullptr ? chosen->GetPrefilterMap() : nullptr,
							brdf_lut);
					}

					const auto& mesh_primitive = mesh_object->MeshSegment->GetMeshPrimitive();

					/* 天空盒与不参与光照合成的材质：单笔原样提交 */
					if (material->IsSkyBox() || !material->SupportsDirectLighting())
					{
						Renderer::Submit(material, mesh_primitive,
							per_draw.Overrides.empty() && !per_draw.RasterStateOverride.has_value()
								? nullptr : &per_draw);
						continue;
					}

					/* 本视图没有光源（都落在看不到的层）：仍画一笔环境光 + IBL，
					 * 否则物体整片消失 */
					if (lights.empty())
					{
						Renderer::FillEmptyLightUniformBuffer();
						per_draw.Overrides.emplace_back(ParamType::Int, "u_ComposeAmbientEmission", 1);
						Renderer::Submit(material, mesh_primitive, &per_draw);
						per_draw.Overrides.pop_back();
						continue;
					}

					for (size_t light_index = 0; light_index < lights.size(); ++light_index)
					{
						Renderer::FillLightUniformBuffer(lights[light_index], shadow_maps);

						/* 合成开关只在环境项那一笔打开；其余笔加覆盖、用后即还（下笔重设） */
						per_draw.Overrides.emplace_back(ParamType::Int, "u_ComposeAmbientEmission",
							light_index == ambient_light_index ? 1 : 0);
						if (light_index > 0)
							per_draw.RasterStateOverride = MakeAdditiveRasterState(base_state);

						Renderer::Submit(material, mesh_primitive, &per_draw);

						per_draw.Overrides.pop_back();
					}
				}
			}
		}

		struct ScenePassData
		{
			FrameGraphResourceHandleTyped<FrameGraphTexture> ShadowMapHandle; /* 阴影贴图 */
		};

		void SubmitSceneObjects(RenderView& render_view, Scene& scene, bool transparent)
		{
			SubmitForwardObjects(render_view, scene, transparent);
		}

		struct ScenePassTextureData
		{
			FrameGraphResourceHandleTyped<FrameGraphTexture> ShadowMapHandle;
			FrameGraphResourceHandleTyped<FrameGraphTexture> Output;   /* rgb: 最终颜色 */
			FrameGraphResourceHandleTyped<FrameGraphTexture> ObjectId; /* 实体 ID（清 -1，供按像素拾取） */
			FrameGraphResourceHandleTyped<FrameGraphTexture> Depth;    /* 供叠加层做深度比较 */
		};

		namespace
		{
			/* 场景 → 视图纹理的公共体（颜色 + ObjectId + 深度三附件；颜色 / 深度落 Blackboard）：
			 * 编辑器视口的前向纹理路径与"默认目标 + 后处理"的旁路共用；调用方负责空指针守卫。 */
			FrameGraphResourceHandleTyped<FrameGraphTexture> AddScenePassToTextureCore(RenderView* render_view)
			{
				auto& frame_graph = render_view->GetFrameGraph();
				auto scene = render_view->GetOwnerScene();

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

							/* 不透明先画（写深度），透明最后（从远到近、只测不写） */
							SubmitForwardObjects(*render_view, *scene, false);
							SubmitForwardObjects(*render_view, *scene, true);
						}
						render_pass_info->Unbind();
						Renderer::GetRenderAPI()->Flush();
					}
					);

				frame_graph->GetBlackboard()["ScenePassOutput"] = scene_pass->GetData().Output;
				frame_graph->GetBlackboard()["ScenePassDepth"] = scene_pass->GetData().Depth;

				return scene_pass->GetData().Output;
			}
		}

		void AddScenePass(RenderView* render_view)
		{
			if (!render_view) return;

			auto& frame_graph = render_view->GetFrameGraph();
			if (!frame_graph) return;

            auto scene = render_view->GetOwnerScene();
            if (!scene) return;

			/* 视图开着后处理（场景里有生效的「后处理对象」）：旁路到纹理 —— 场景 → 视图纹理 →
			 * 后处理链 → 呈现 Pass 全屏画回默认目标；否则照旧直渲（"关闭 = 零改动"基线）。
			 * 调试视图非 None 时 HasEnabledEffects 恒假（与纹理路径的"调试视图直通"同一语义）。 */
			if (PostProcess::HasEnabledEffects(*render_view))
			{
				const auto scene_color = AddScenePassToTextureCore(render_view);
				const auto final_color = PostProcess::AddPostProcessPasses(*render_view, scene_color);
				PostProcess::AddPresentToDefaultTarget(*render_view, final_color);
				return;
			}

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

						/* 不透明先画（写深度），透明最后（从远到近、只测不写） */
						SubmitForwardObjects(*render_view, *scene, false);
						SubmitForwardObjects(*render_view, *scene, true);
					}
				}
			);
		}

		void AddScenePassToTexture(RenderView* render_view)
		{
			if (!render_view) return;
			if (!render_view->GetFrameGraph()) return;
			if (!render_view->GetOwnerScene()) return;

			const auto scene_color = AddScenePassToTextureCore(render_view);

			/* 调试视图（几何重绘 / Overdraw 档位）：场景之后、叠加层之前 ——
			 * 前向没有独立天空 Pass，几何调试的背景即清屏色（延迟由天空收尾） */
			DebugView::AddDebugViewPasses(*render_view, { scene_color, false });

			/* 后处理链（效果注册表驱动）：无效果 / 未启用 = 恒直通 */
			const auto final_output = PostProcess::AddPostProcessPasses(*render_view, scene_color);
			render_view->SetRenderTargetHandle(final_output);
		}
	}
}
