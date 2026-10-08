#include "Pch.h"
#include "DeferredPasses.h"
#include <Helios/Application/AssetManager.h>
#include <Helios/Renderer/FrameGraph/FrameGraph.h>
#include <Helios/Renderer/Renderer.h>
#include <Helios/Renderer/RenderView.h>
#include <Helios/Scene/Camera.h>
#include <Helios/Scene/Material.h>
#include <Helios/Scene/Mesh.h>
#include <Helios/Scene/ReflectionProbe.h>
#include <Helios/Scene/Scene.h>
#include <Helios/Scene/SceneCommon.h>
#include <Helios/VirtualDevice/DeviceShader.h>
#include <Helios/VirtualDevice/DeviceTexture.h>

namespace Helios
{
	namespace
	{
		/* 延迟光照一次参与"逐像素最近探针"选择的探针上限：受纹理 / 采样器槽位
		 * 预算约束（DeferredShaders/Lighting.glsl 里每个探针占一组固定绑定点）。
		 * 场景内探针多于上限时取离相机最近的若干个（见 CollectClosestBakedProbes）。 */
		constexpr size_t kMaxDeferredIBLProbes = 3;

		/* G-Buffer 采样点缺省时补的默认贴图：法线图按名字认；乘法类（Albedo / Roughness / Ambient）
		 * 用白，其余用黑。注意：Ambient 不能用黑 —— 缺贴图的材质走 "albedo × 0.3" 回退环境项，
		 * 黑贴图会让延迟端在没有探针时把这个环境项丢掉。 */
		SharedPtr<DeviceTexture> DefaultTextureForSampler(const std::string& sampler_name)
		{
			if (sampler_name.find("Normal") != std::string::npos)
				return TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH("Textures/normal.png"));
			if (sampler_name.find("Albedo") != std::string::npos
				|| sampler_name.find("Roughness") != std::string::npos
				|| sampler_name.find("Ambient") != std::string::npos)
				return TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH("Textures/White.png"));
			return TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH("Textures/Black.png"));
		}

		SharedPtr<Material> CreateGBufferMaterial(
			const SharedPtr<Material>& source, const SharedPtr<DeviceShader>& gbuffer_shader)
		{
			auto material = Material::Create(gbuffer_shader);
			for (const auto& [_, parameter] : source->GetAllParameters())
			{
				if (parameter.Type == ParamType::Texture)
				{
					if (gbuffer_shader->GetUniformBinding(parameter.Name) < 0)
						continue;
					const auto texture = std::any_cast<std::pair<SharedPtr<DeviceTexture>, uint32_t>>(parameter.Value);
					material->SetTexture(parameter.Name, texture.first);
				}
				else
				{
					material->SetParameters(parameter.Type, parameter.Name, parameter.Value);
				}
			}

			/* 源材质没覆盖到的采样点补默认贴图：GBufferMaterial.glsl 的采样点必须全部有绑定，
			 * 缺一张就是未定义输入（Metal 校验层会直接断言）。参数不全的老材质、新建材质
			 * 都靠这一步在编辑器里兜住 —— 直接遍历目标 shader 的反射列表，与 shader 声明保持同步。 */
			for (const auto& sampler_entry : gbuffer_shader->GetReflectionData().SamplerBindings)
			{
				const std::string& sampler_name = sampler_entry.first;
				if (material->GetAllParameters().find(ToID(sampler_name)) != material->GetAllParameters().end())
					continue;
				material->SetTexture(sampler_name, DefaultTextureForSampler(sampler_name));
			}

			material->SetRasterState(source->GetRasterState());
			return material;
		}
	}

	namespace Deferred
	{
		namespace
		{
			/* GBuffer Pass：可见对象渲染进 MRT（6 张 G-Buffer 纹理 + ObjectId + 深度）。
			 * 普通材质在提交前转换为 GBufferMaterial.glsl 对应的临时材质。 */
			void AddGBufferPass(RenderView& render_view)
			{
				PROFILE_FUNCTION();

				auto& frame_graph = render_view.GetFrameGraph();
				const auto& viewport_region = render_view.GetViewportRegion();

				FrameGraphTexture::Descriptor color_target_desc;
				color_target_desc.Width = viewport_region.Width;
				color_target_desc.Height = viewport_region.Height;
				color_target_desc.TextureFormat = TextureFormat::RGBA8;

				/* 世界位置通道用浮点格式：RGBA8 会把世界坐标钳制到 [0,1]（负值归零、
				 * 大于 1 截平），光照阶段拿它做级联阴影变换时采样点全部落在错误位置 ——
				 * 阴影与场景对不上（偏移、截断，甚至全域误判为受光/被遮）。 */
				FrameGraphTexture::Descriptor world_pos_target_desc = color_target_desc;
				world_pos_target_desc.TextureFormat = TextureFormat::RGBA32F;

				FrameGraphTexture::Descriptor depth_target_desc;
				depth_target_desc.Width = viewport_region.Width;
				depth_target_desc.Height = viewport_region.Height;
				depth_target_desc.TextureFormat = TextureFormat::Depth32;

				FrameGraphTexture::Descriptor object_id_desc;
				object_id_desc.Width = viewport_region.Width;
				object_id_desc.Height = viewport_region.Height;
				object_id_desc.TextureFormat = TextureFormat::R32I;

				/* GBuffer Pass 使用 GBufferMaterial.glsl；普通材质在提交前转换为该 shader 对应的临时材质。 */
				const auto gbuffer_shader = ShaderAssetManager::Instance().GetOrLoad(
					ABSOLUTE_PATH("Shaders/DeferredShaders/GBufferMaterial.glsl"));

				struct GBufferPassData
				{
					FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture0;
					FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture1;
					FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture2;
					FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture3;
					FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture4;
					FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture5;
					FrameGraphResourceHandleTyped<FrameGraphTexture> ObjectId;
					FrameGraphResourceHandleTyped<FrameGraphTexture> Depth;
				};

				auto gbuffer_pass = frame_graph->AddPass<GBufferPassData>("GBufferPass",
					[&, gbuffer_shader](FrameGraphBuilder& builder, GBufferPassData& data)
					{
						data.GBufferTexture0 = builder.CreateTexture("GBufferTexture0", color_target_desc);
						data.GBufferTexture1 = builder.CreateTexture("GBufferTexture1", color_target_desc);
						data.GBufferTexture2 = builder.CreateTexture("GBufferTexture2", color_target_desc);
						data.GBufferTexture3 = builder.CreateTexture("GBufferTexture3", color_target_desc);
						data.GBufferTexture4 = builder.CreateTexture("GBufferTexture4", color_target_desc);
						data.GBufferTexture5 = builder.CreateTexture("GBufferTexture5", world_pos_target_desc);
						data.ObjectId = builder.CreateTexture("ObjectId", object_id_desc);
						data.Depth = builder.CreateTexture("GBufferDepth", depth_target_desc);

						builder.BindOutputResource(data.GBufferTexture0, FrameGraphTexture::Usage::ColorAttachment);
						builder.BindOutputResource(data.GBufferTexture1, FrameGraphTexture::Usage::ColorAttachment);
						builder.BindOutputResource(data.GBufferTexture2, FrameGraphTexture::Usage::ColorAttachment);
						builder.BindOutputResource(data.GBufferTexture3, FrameGraphTexture::Usage::ColorAttachment);
						builder.BindOutputResource(data.GBufferTexture4, FrameGraphTexture::Usage::ColorAttachment);
						builder.BindOutputResource(data.GBufferTexture5, FrameGraphTexture::Usage::ColorAttachment);
						builder.BindOutputResource(data.ObjectId, FrameGraphTexture::Usage::ColorAttachment | FrameGraphTexture::Usage::Sampleable);
						builder.BindOutputResource(data.Depth, FrameGraphTexture::Usage::DepthAttachment);

						FrameGraphPassInfo::Descriptor pass_desc;
						pass_desc.Attachments.ColorAttachments(0) = data.GBufferTexture0;
						pass_desc.Attachments.ColorAttachments(1) = data.GBufferTexture1;
						pass_desc.Attachments.ColorAttachments(2) = data.GBufferTexture2;
						pass_desc.Attachments.ColorAttachments(3) = data.GBufferTexture3;
						pass_desc.Attachments.ColorAttachments(4) = data.GBufferTexture4;
						pass_desc.Attachments.ColorAttachments(5) = data.GBufferTexture5;
						pass_desc.Attachments.ColorAttachments(6) = data.ObjectId;
						pass_desc.Attachments.DepthAttachment() = data.Depth;
						pass_desc.ColorClearValues.resize(7);
						pass_desc.ColorClearValues[6] = glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f);
						pass_desc.ViewportRegion = viewport_region;
						builder.CreateRenderPass("GBufferPassRenderTarget", pass_desc);
					},
					[&render_view, gbuffer_shader](const FrameGraphResources& resources, const GBufferPassData& data)
					{
						std::unordered_map<const Material*, SharedPtr<Material>> gbuffer_materials;
						/* 目标 shader 已声明、材质没写的值参数 → 用默认值补齐。块式 uniform 是 per-program 状态：
						 * 不写会残留上一个对象的取值。 */
						std::unordered_map<const Material*, std::vector<MaterialParamInfo>> gbuffer_param_fills;
						const auto render_pass_info = resources.GetPassRenderTarget();
						render_view.EmplacePassFrameBuffer("GBufferPass", render_pass_info);

						render_pass_info->Bind();
						{
							Renderer::GetRenderAPI()->Clear();

							for (const auto& mesh_object : render_view.GetVisibleMeshObjects())
							{
								/* Fill object uniform buffer */
								Renderer::FillObjectUniformBuffer(mesh_object);

								const auto& material = mesh_object.Material;
								if (material == nullptr)
									continue;

								/* 天空盒不进 G-Buffer：它输出的是最终颜色、顶点着色器把网格
								 * 折叠成全屏天空，转成 G-Buffer 材质只会得到一颗实心球。
								 * 由 SkyPass 在光照之后专绘背景。 */
								if (material->IsSkyBox())
									continue;

								auto gbuffer_material = material;
								if (material->GetShader() != gbuffer_shader)
								{
									auto [cached, inserted] = gbuffer_materials.try_emplace(material.get());
									if (inserted)
										cached->second = CreateGBufferMaterial(material, gbuffer_shader);
									gbuffer_material = cached->second;
								}

								/* 按材质对象缓存补齐结果（转换材质是源材质的复制，参数同源） */
								auto fills = gbuffer_param_fills.try_emplace(material.get());
								if (fills.second)
									fills.first->second = gbuffer_material->CollectMissingValueParamDefaults();

								DrawParams per_draw;
								per_draw.Overrides = fills.first->second;
								Renderer::Submit(gbuffer_material, mesh_object.MeshSegment->GetMeshPrimitive(),
									per_draw.Overrides.empty() ? nullptr : &per_draw);
							}
						}
						render_pass_info->Unbind();
						Renderer::GetRenderAPI()->Flush();
					}
					);

				frame_graph->GetBlackboard()["GBufferTexture0"] = gbuffer_pass->GetData().GBufferTexture0;
				frame_graph->GetBlackboard()["GBufferTexture1"] = gbuffer_pass->GetData().GBufferTexture1;
				frame_graph->GetBlackboard()["GBufferTexture2"] = gbuffer_pass->GetData().GBufferTexture2;
				frame_graph->GetBlackboard()["GBufferTexture3"] = gbuffer_pass->GetData().GBufferTexture3;
				frame_graph->GetBlackboard()["GBufferTexture4"] = gbuffer_pass->GetData().GBufferTexture4;
				frame_graph->GetBlackboard()["GBufferTexture5"] = gbuffer_pass->GetData().GBufferTexture5;
				frame_graph->GetBlackboard()["GBufferDepth"] = gbuffer_pass->GetData().Depth;
			}

			/* Lighting Pass：全屏逐光源加法合成，输出即视图最终颜色（天空与叠加层都写进它本身）。
			 * 返回光照结果句柄（视图输出）。 */
			FrameGraphResourceHandle AddLightingPass(RenderView& render_view)
			{
				PROFILE_FUNCTION();

				auto& frame_graph = render_view.GetFrameGraph();
				const auto& viewport_region = render_view.GetViewportRegion();

				FrameGraphTexture::Descriptor color_target_desc;
				color_target_desc.Width = viewport_region.Width;
				color_target_desc.Height = viewport_region.Height;
				color_target_desc.TextureFormat = TextureFormat::RGBA8;

				struct LightingPassData
				{
					FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture0;
					FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture1;
					FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture2;
					FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture3;
					FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture4;
					FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture5;
					FrameGraphResourceHandleTyped<FrameGraphTexture> ShadowMapHandle;
					FrameGraphResourceHandleTyped<FrameGraphTexture> LightingResult;
				};

				auto lighting_pass = frame_graph->AddPass<LightingPassData>("LightingPass",
					[&](FrameGraphBuilder& builder, LightingPassData& data)
					{
						data.GBufferTexture0 = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("GBufferTexture0");
						data.GBufferTexture1 = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("GBufferTexture1");
						data.GBufferTexture2 = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("GBufferTexture2");
						data.GBufferTexture3 = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("GBufferTexture3");
						data.GBufferTexture4 = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("GBufferTexture4");
						data.GBufferTexture5 = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("GBufferTexture5");
						data.ShadowMapHandle = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("ShadowMapHandle");

						data.LightingResult = builder.CreateTexture("LightingResult", color_target_desc);

						builder.BindInputResource(data.GBufferTexture0, FrameGraphTexture::Usage::Sampleable);
						builder.BindInputResource(data.GBufferTexture1, FrameGraphTexture::Usage::Sampleable);
						builder.BindInputResource(data.GBufferTexture2, FrameGraphTexture::Usage::Sampleable);
						builder.BindInputResource(data.GBufferTexture3, FrameGraphTexture::Usage::Sampleable);
						builder.BindInputResource(data.GBufferTexture4, FrameGraphTexture::Usage::Sampleable);
						builder.BindInputResource(data.GBufferTexture5, FrameGraphTexture::Usage::Sampleable);
						builder.BindInputResource(data.ShadowMapHandle, FrameGraphTexture::Usage::Sampleable);
						builder.BindOutputResource(data.LightingResult, FrameGraphTexture::Usage::ColorAttachment | FrameGraphTexture::Usage::Sampleable);

						FrameGraphPassInfo::Descriptor pass_desc;
						pass_desc.Attachments.ColorAttachments(0) = data.LightingResult;
						pass_desc.ViewportRegion = viewport_region;
						builder.CreateRenderPass("LightingPassRenderTarget", pass_desc);

						builder.AsSideEffect();
					},
					[&render_view](const FrameGraphResources& resources, const LightingPassData& data)
					{
						const auto render_pass_info = resources.GetPassRenderTarget();
						render_view.EmplacePassFrameBuffer("LightingPass", render_pass_info);

						render_pass_info->Bind();
						{
							Renderer::GetRenderAPI()->Clear();

							/* 阴影纹理随 Submit 直绑（按 Shader 反射出的 "u_ShadowMap" 绑定点） */
							ScopedShadowMapBinding shadow_map_binding(resources.Get(data.ShadowMapHandle).Texture);

							/* 反射探针 IBL：收集本帧参与"逐像素最近探针"选择的探针子集
							 * （≤ kMaxDeferredIBLProbes 个，按到相机的距离排序）。
							 * BRDF LUT 未就绪（没有探针烘焙过）时不做 IBL。 */
							std::vector<SharedPtr<ReflectionProbe>> ibl_probes;
							SharedPtr<DeviceTexture> ibl_brdf_lut;
							if (const auto scene = render_view.GetOwnerScene())
							{
								if (const auto probe_manager = scene->GetReflectionProbeManager())
								{
									ibl_brdf_lut = probe_manager->GetBRDFLutMap();
									if (ibl_brdf_lut != nullptr)
										ibl_probes = probe_manager->CollectClosestBakedProbes(
											render_view.GetCullingCamera()->GetPosition(), kMaxDeferredIBLProbes);
								}
							}

							const auto& lights = render_view.GetValidLights();
							for (size_t light_index = 0; light_index < lights.size(); ++light_index)
							{
								const auto& light = lights[light_index];

								/* 完整光照数据：光源参数 + 该光源的阴影（级联 / 点光 / 聚光，来自本视图阴影管理器） */
								Renderer::FillLightUniformBuffer(light, render_view.GetShadowMapManager().get());

								SharedPtr<Material> material = CreateSharedPtr<Material>();
								auto& raster_state = material->GetRasterState();
								raster_state.EnableDepthWrite = false;
								raster_state.EnableBlend = true;
								raster_state.BlendEquationRGB = BlendEquation::Add;
								raster_state.BlendEquationA = BlendEquation::Add;
								raster_state.BlendFuncSrcRGB = BlendFunc::One;
								raster_state.BlendFuncSrcA = BlendFunc::One;
								raster_state.BlendFuncDstRGB = BlendFunc::One;
								raster_state.BlendFuncDstA = BlendFunc::One;
								const auto shader = ShaderAssetManager::Instance().GetOrLoad(
									ABSOLUTE_PATH("Shaders/DeferredShaders/Lighting.glsl"));
								material->SetShader(shader);
								material->SetTexture("u_GBufferTexture0", resources.Get(data.GBufferTexture0).Texture);
								material->SetTexture("u_GBufferTexture1", resources.Get(data.GBufferTexture1).Texture);
								material->SetTexture("u_GBufferTexture2", resources.Get(data.GBufferTexture2).Texture);
								material->SetTexture("u_GBufferTexture3", resources.Get(data.GBufferTexture3).Texture);
								material->SetTexture("u_GBufferTexture4", resources.Get(data.GBufferTexture4).Texture);
								material->SetTexture("u_GBufferTexture5", resources.Get(data.GBufferTexture5).Texture);
								/* 环境光 / 自发光与光源无关，只由第一笔光照合成（多光源逐笔加法叠加） */
								material->SetParameters(ParamType::Int, "u_ComposeAmbientEmission",
									light_index == 0 ? 1 : 0);

								/* 探针 IBL 的参数和贴图每笔光照都完整设一遍（裸 uniform 是 per-program 状态，漏设就残留
								 * 上一笔）；采样槽位不管有没有效都必须有绑定（缺绑定会被 Metal 校验断言），无效槽位用中性兜底。 */
								material->SetParameters(ParamType::Int, "u_ProbeCount",
									static_cast<int>(ibl_probes.size()));
								for (size_t probe_index = 0; probe_index < kMaxDeferredIBLProbes; ++probe_index)
								{
									const std::string suffix = std::to_string(probe_index);
									const bool valid = probe_index < ibl_probes.size();
									material->SetParameters(ParamType::Vec3, "u_ProbePosition" + suffix,
										valid ? ibl_probes[probe_index]->GetPosition() : glm::vec3(0.0f));

									material->SetTexture("u_IrradianceMap" + suffix,
										valid ? ibl_probes[probe_index]->GetIrradianceMap() : DeviceTexture::BlackCube());
									material->SetTexture("u_PrefilterMap" + suffix,
										valid ? ibl_probes[probe_index]->GetPrefilterMap() : DeviceTexture::BlackCube());
								}
								material->SetTexture("u_BRDFLut",
									ibl_brdf_lut != nullptr ? ibl_brdf_lut : DeviceTexture::Black());

								Renderer::Submit(material, Renderer::GetFullScreenVertexArray());
							}
						}
						render_pass_info->Unbind();
					}
					);
				frame_graph->GetBlackboard()["LightingPassOutput"] = lighting_pass->GetData().LightingResult;

				return lighting_pass->GetData().LightingResult;
			}

			/* 天空背景 Pass：天空盒材质按原样画进光照结果。顶点着色器折叠成全屏天空、深度恒为远平面
			 * （Reversed-Z），跟 GBuffer 深度做 GreaterEqual 比较（只在背景像素落笔）。要在光照之后、
			 * 叠加层之前。 */
			void AddSkyPass(RenderView& render_view)
			{
				PROFILE_FUNCTION();

				auto& frame_graph = render_view.GetFrameGraph();
				const auto& viewport_region = render_view.GetViewportRegion();

				struct SkyPassData
				{
					FrameGraphResourceHandleTyped<FrameGraphTexture> Output;
					FrameGraphResourceHandleTyped<FrameGraphTexture> Depth;
				};

				frame_graph->AddPass<SkyPassData>("SkyPass",
					[&](FrameGraphBuilder& builder, SkyPassData& data)
					{
						data.Output = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("LightingPassOutput");
						data.Depth = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("GBufferDepth");
						builder.BindInputResource(data.Output, FrameGraphTexture::Usage::ColorAttachment);
						builder.BindInputResource(data.Depth, FrameGraphTexture::Usage::DepthAttachment);

						FrameGraphPassInfo::Descriptor pass_desc;
						pass_desc.Attachments.ColorAttachments(0) = data.Output;
						pass_desc.Attachments.DepthAttachment() = data.Depth;
						/* 保留光照结果与场景深度（天空只在背景像素落笔） */
						pass_desc.PreserveContent = true;
						pass_desc.ViewportRegion = viewport_region;
						builder.CreateRenderPass("SkyPassRenderTarget", pass_desc);

						/* 与叠加型 Pass 同理：产物是"写进已有资源"，
						 * 不声明为目标会被图裁剪剔除 */
						builder.AsSideEffect();
					},
					[&render_view](const FrameGraphResources& resources, const SkyPassData&)
					{
						const auto render_pass_info = resources.GetPassRenderTarget();
						render_view.EmplacePassFrameBuffer("SkyPass", render_pass_info);

						render_pass_info->Bind();
						{
							/* 天空盒材质按原样提交（不做 G-Buffer 转换）：u_SkyTex 采样、
							 * 远平面 z、面剔除姿态都由材质自身的光栅状态决定 */
							for (const auto& mesh_object : render_view.GetVisibleMeshObjects())
							{
								const auto& material = mesh_object.Material;
								if (material == nullptr || !material->IsSkyBox())
									continue;

								Renderer::FillObjectUniformBuffer(mesh_object);
								Renderer::Submit(material, mesh_object.MeshSegment->GetMeshPrimitive());
							}
						}
						render_pass_info->Unbind();
					}
					);
			}
		}

		void AddDeferredPasses(RenderView* render_view)
		{
			PROFILE_FUNCTION();

			if (render_view == nullptr)
				return;

			/* 视口尺寸无效时不组织渲染图（0 尺寸纹理创建会失败；空图不执行任何 Pass）。 */
			const auto& viewport_region = render_view->GetViewportRegion();
			if (viewport_region.Width == 0 || viewport_region.Height == 0)
				return;

			/* 阴影前置（中性阴影图 + 视情况的真实阴影图）：光照着色器总要能采样到合法纹理 */
			render_view->AddShadowMapPasses();

			AddGBufferPass(*render_view);
			const auto lighting_output = AddLightingPass(*render_view);
			AddSkyPass(*render_view);

			/* 延迟视图的输出 = 光照结果（天空与叠加层都画进它本身） */
			render_view->SetRenderTargetHandle(lighting_output);
		}
	}
}
