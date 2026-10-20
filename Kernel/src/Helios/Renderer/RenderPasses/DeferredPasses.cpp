#include "Pch.h"
#include "DeferredPasses.h"
#include "DebugViewPasses.h"
#include "ObjectSorting.h"
#include "ScenePass.h"
#include <Helios/Core/AssetManager.h>
#include <Helios/Renderer/FrameGraph/FrameGraph.h>
#include <Helios/Renderer/PostProcess/PostProcessPasses.h>
#include <Helios/Renderer/Renderer.h>
#include <Helios/Renderer/RenderView.h>
#include <Helios/Scene/Camera.h>
#include <Helios/Scene/Light.h>
#include <Helios/Scene/Material.h>
#include <Helios/Scene/Mesh.h>
#include <Helios/Scene/ReflectionProbe.h>
#include <Helios/Scene/Scene.h>
#include <Helios/Scene/SceneCommon.h>
#include <Helios/Scene/ShadowMap.h>
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

		/* 光照阶段认识的调试档位（Lighting 类）：只对它们下发档位号，
		 * 其余档位（Surface / Analysis / Shadow 类）统一下发 0 = 正常着色。
		 * Shadow 档要叠加多盏灯，走自己的累加 Pass（见 DebugViewPasses）。 */
		int LightingDebugViewValue(DebugViewMode mode)
		{
			switch (mode)
			{
			case DebugViewMode::Diffuse:
			case DebugViewMode::Specular:
			case DebugViewMode::IndirectDiffuse:
			case DebugViewMode::IndirectSpecular:
			case DebugViewMode::Probe:
				return static_cast<int>(mode);
			case DebugViewMode::None:
			case DebugViewMode::Albedo:
			case DebugViewMode::Normal:
			case DebugViewMode::Roughness:
			case DebugViewMode::Metallic:
			case DebugViewMode::SpecularColor:
			case DebugViewMode::AmbientOcclusion:
			case DebugViewMode::Emission:
			case DebugViewMode::Ambient:
			case DebugViewMode::Shadow:
			case DebugViewMode::Overdraw:
			case DebugViewMode::Mipmap:
			case DebugViewMode::Skeleton:
			case DebugViewMode::Wireframe:
				return 0;
			}
			return 0;
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

			/* 源材质没覆盖到的采样点补默认贴图：GBuffer.glsl 的采样点必须全部有绑定，
			 * 缺一张就是未定义输入（Metal 校验层会直接断言）。参数不全的老材质、新建材质
			 * 都靠这一步在编辑器里兜住 —— 直接遍历目标 shader 的反射列表，与 shader 声明保持同步。 */
			for (const auto& sampler_entry : gbuffer_shader->GetReflectionData().SamplerBindings)
			{
				const std::string& sampler_name = sampler_entry.first;
				if (material->GetAllParameters().find(ToID(sampler_name)) != material->GetAllParameters().end())
					continue;
				material->SetTexture(sampler_name, Material::DefaultTextureForSampler(sampler_name));
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
			 * 普通材质在提交前转换为 GBuffer.glsl 对应的临时材质。 */
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
				/* 实体 id 用浮点存：多重采样只对浮点 / 颜色格式做解析，
				 * 整数附件在开启 MSAA 时解析结果不可靠 */
				object_id_desc.TextureFormat = TextureFormat::R32F;

				/* GBuffer Pass 使用 GBuffer.glsl；普通材质在提交前转换为该 shader 对应的临时材质。 */
				const auto gbuffer_shader = AssetManager::Instance().GetOrLoad<DeviceShader>(
					ABSOLUTE_PATH("Shaders/DeferredShaders/GBuffer.glsl"));

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
						/* G-Buffer 不开多重采样：轮廓像素会把空采样（清零的法线 / 世界坐标）
						 * 一起平均进来，光照拿到假数据，边缘冒白边。延迟的抗锯齿交给后处理。 */
						pass_desc.Multisample = false;
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
								 * 由 SkyPass 在光照之后专绘背景。
								 * 透明物体也不进：G-Buffer 只存单层表面，叠不出半透明，
								 * 统一放到光照之后的前向透明 Pass 里画。 */
								if (material->IsSkyBox() || material->IsTransparent())
									continue;

								auto gbuffer_material = material;
								/* 声称用 GBuffer shader 但采样点没配全的老材质（head.mtl 这类待迁移
								 * 资产）也要走转换：直画会带着未绑定的采样点，画面是垃圾输入 */
								bool needs_conversion = (material->GetShader() != gbuffer_shader);
								if (!needs_conversion)
								{
									const auto& parameters = material->GetAllParameters();
									for (const auto& sampler_entry : gbuffer_shader->GetReflectionData().SamplerBindings)
									{
										const auto iter = parameters.find(ToID(sampler_entry.first));
										if (iter == parameters.end() || iter->second.Type != ParamType::Texture)
										{
											needs_conversion = true;
											break;
										}
									}
								}
								if (needs_conversion)
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
				frame_graph->GetBlackboard()["GBufferObjectId"] = gbuffer_pass->GetData().ObjectId;
				frame_graph->GetBlackboard()["GBufferDepth"] = gbuffer_pass->GetData().Depth;
			}

			/* Lighting Pass：全屏逐光源加法合成，输出即视图最终颜色（天空与叠加层都写进它本身）。
			 * 返回光照结果句柄（视图输出）。 */
			FrameGraphResourceHandleTyped<FrameGraphTexture> AddLightingPass(RenderView& render_view)
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
							bool ibl_blend_enabled = false;
							if (const auto scene = render_view.GetOwnerScene())
							{
								if (const auto probe_manager = scene->GetReflectionProbeManager())
								{
									ibl_brdf_lut = probe_manager->GetBRDFLutMap();
									if (ibl_brdf_lut != nullptr)
									{
										ibl_probes = probe_manager->CollectClosestBakedProbes(
											render_view.GetCullingCamera()->GetPosition(), kMaxDeferredIBLProbes,
											render_view.GetCullingMask());
										ibl_blend_enabled = probe_manager->IsProbeBlendingEnabled(
											render_view.GetCullingMask());
									}
								}
							}

							const auto& lights = render_view.GetValidLights();

							/* 主阴影光源：环境项只在它的那一笔合成 —— 其它笔没有阴影数据，
							 * 环境项跟着它们会收不到阴影 */
							size_t primary_shadow_light = 0;
							if (const auto& shadow_maps = render_view.GetShadowMapManager())
								primary_shadow_light = shadow_maps->FindPrimaryShadowLightIndex(lights);

							const int debug_view_value = LightingDebugViewValue(render_view.GetDebugViewMode());

							/* 单笔光照：light 为空时按"无光源"填 UBO（只合成环境光 + IBL） */
							const auto draw_light = [&](const SharedPtr<Light>& light, size_t light_index)
							{
								if (light != nullptr)
								{
									/* 完整光照数据：光源参数 + 该光源的阴影（级联 / 点光 / 聚光，来自本视图阴影管理器） */
									Renderer::FillLightUniformBuffer(light, render_view.GetShadowMapManager().get());
								}
								else
								{
									Renderer::FillEmptyLightUniformBuffer();
								}

								SharedPtr<Material> material = CreateSharedPtr<Material>();
								auto& raster_state = material->GetRasterState();
								raster_state.EnableDepthTest = false;
								raster_state.EnableDepthWrite = false;
								raster_state.EnableBlend = true;
								raster_state.BlendEquationRGB = BlendEquation::Add;
								raster_state.BlendEquationA = BlendEquation::Add;
								raster_state.BlendFuncSrcRGB = BlendFunc::One;
								raster_state.BlendFuncSrcA = BlendFunc::One;
								raster_state.BlendFuncDstRGB = BlendFunc::One;
								raster_state.BlendFuncDstA = BlendFunc::One;
								const auto shader = AssetManager::Instance().GetOrLoad<DeviceShader>(
									ABSOLUTE_PATH("Shaders/DeferredShaders/Lighting.glsl"));
								material->SetShader(shader);
								material->SetTexture("u_GBufferTexture0", resources.Get(data.GBufferTexture0).Texture);
								material->SetTexture("u_GBufferTexture1", resources.Get(data.GBufferTexture1).Texture);
								material->SetTexture("u_GBufferTexture2", resources.Get(data.GBufferTexture2).Texture);
								material->SetTexture("u_GBufferTexture3", resources.Get(data.GBufferTexture3).Texture);
								material->SetTexture("u_GBufferTexture4", resources.Get(data.GBufferTexture4).Texture);
								material->SetTexture("u_GBufferTexture5", resources.Get(data.GBufferTexture5).Texture);
								/* 环境光 / 自发光与光源无关，只在主阴影光源那一笔合成（多光源逐笔加法叠加） */
								material->SetParameters(ParamType::Int, "u_ComposeAmbientEmission",
									light_index == primary_shadow_light ? 1 : 0);
								/* 调试视图：光照分量档位（Diffuse / Specular / 环境项 / Probe） */
								material->SetParameters(ParamType::Int, "u_DebugView", debug_view_value);

								/* 探针 IBL 的参数和贴图每笔光照都完整设一遍（裸 uniform 是 per-program 状态，漏设就残留
								 * 上一笔）；采样槽位不管有没有效都必须有绑定（缺绑定会被 Metal 校验断言），无效槽位用中性兜底。 */
								material->SetParameters(ParamType::Int, "u_ProbeCount",
									static_cast<int>(ibl_probes.size()));
								/* 混合开关：没有探针配过渡时，着色器走旧的"距离最近"路径 */
								material->SetParameters(ParamType::Int, "u_ProbeBlendEnabled",
									ibl_blend_enabled ? 1 : 0);
								for (size_t probe_index = 0; probe_index < kMaxDeferredIBLProbes; ++probe_index)
								{
									const std::string suffix = std::to_string(probe_index);
									const bool valid = probe_index < ibl_probes.size();
									material->SetParameters(ParamType::Vec3, "u_ProbePosition" + suffix,
										valid ? ibl_probes[probe_index]->GetPosition() : glm::vec3(0.0f));
									/* 盒体参数：xyz = 半尺寸、w = 混合距离（关闭混合时不读） */
									material->SetParameters(ParamType::Vec4, "u_ProbeExtent" + suffix,
										valid ? glm::vec4(ibl_probes[probe_index]->GetBoxSize() * 0.5f,
											ibl_probes[probe_index]->GetBlendDistance())
										: glm::vec4(0.0f));

									material->SetTexture("u_IrradianceMap" + suffix,
										valid ? ibl_probes[probe_index]->GetIrradianceMap() : DeviceTexture::BlackCube());
									material->SetTexture("u_PrefilterMap" + suffix,
										valid ? ibl_probes[probe_index]->GetPrefilterMap() : DeviceTexture::BlackCube());
								}
								material->SetTexture("u_BRDFLut",
									ibl_brdf_lut != nullptr ? ibl_brdf_lut : DeviceTexture::Black());

								Renderer::Submit(material, Renderer::GetFullScreenVertexArray());
							};

							/* 光源全被层剔掉：补一笔无光源的环境 / IBL 合成，别让画面空掉 */
							if (lights.empty())
								draw_light(nullptr, 0);
							for (size_t light_index = 0; light_index < lights.size(); ++light_index)
								draw_light(lights[light_index], light_index);
						}
						render_pass_info->Unbind();
					}
					);
				frame_graph->GetBlackboard()["LightingPassOutput"] = lighting_pass->GetData().LightingResult;

				return lighting_pass->GetData().LightingResult;
			}

			/* 前向透明 Pass：G-Buffer 只存得住单层表面，透明物体在这里用自身着色器直画 ——
			 * 颜色叠在光照结果（已含天空）上，深度取 G-Buffer 的场景深度（只测不写），
			 * ObjectId 一并写入，像素拾取跟前向管线一致。 */
			void AddForwardTransparentPass(RenderView& render_view)
			{
				PROFILE_FUNCTION();

				auto& frame_graph = render_view.GetFrameGraph();
				const auto& viewport_region = render_view.GetViewportRegion();

				struct TransparentPassData
				{
					FrameGraphResourceHandleTyped<FrameGraphTexture> Output;
					FrameGraphResourceHandleTyped<FrameGraphTexture> ObjectId;
					FrameGraphResourceHandleTyped<FrameGraphTexture> Depth;
					FrameGraphResourceHandleTyped<FrameGraphTexture> ShadowMapHandle;
				};

				frame_graph->AddPass<TransparentPassData>("ForwardTransparentPass",
					[&](FrameGraphBuilder& builder, TransparentPassData& data)
					{
						data.Output = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("LightingPassOutput");
						data.ObjectId = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("GBufferObjectId");
						data.Depth = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("GBufferDepth");
						data.ShadowMapHandle = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("ShadowMapHandle");

						builder.BindInputResource(data.Output, FrameGraphTexture::Usage::ColorAttachment);
						builder.BindInputResource(data.ObjectId, FrameGraphTexture::Usage::ColorAttachment);
						builder.BindInputResource(data.Depth, FrameGraphTexture::Usage::DepthAttachment);
						builder.BindInputResource(data.ShadowMapHandle, FrameGraphTexture::Usage::Sampleable);

						FrameGraphPassInfo::Descriptor pass_desc;
						pass_desc.Attachments.ColorAttachments(0) = data.Output;
						pass_desc.Attachments.ColorAttachments(1) = data.ObjectId;
						pass_desc.Attachments.DepthAttachment() = data.Depth;
						/* 颜色、ObjectId 与场景深度都保留：这个 Pass 只往上叠透明层 */
						pass_desc.PreserveContent = true;
						pass_desc.ViewportRegion = viewport_region;
						builder.CreateRenderPass("ForwardTransparentRenderTarget", pass_desc);

						/* 产物是"写进已有资源"，不声明为目标会被图裁剪剔除 */
						builder.AsSideEffect();
					},
					[&render_view](const FrameGraphResources& resources, const TransparentPassData& data)
					{
						auto scene = render_view.GetOwnerScene();
						if (scene == nullptr)
							return;

						const auto render_pass_info = resources.GetPassRenderTarget();
						render_view.EmplacePassFrameBuffer("ForwardTransparentPass", render_pass_info);

						render_pass_info->Bind();
						{
							ScopedShadowMapBinding shadow_map_binding(resources.Get(data.ShadowMapHandle).Texture);
							Forward::SubmitSceneObjects(render_view, *scene, true);
						}
						render_pass_info->Unbind();
					}
					);
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
			/* 调试视图（合成 / Overdraw / 几何重绘档位）：光照之后、天空之前 ——
			 * 先落笔、天空最后负责背景像素（线框档除外，见下）；Lighting 类档位在光照阶段内分流，此处空操作 */
			DebugView::AddDebugViewPasses(*render_view, { lighting_output, true });
			/* 线框档要的是纯结构视图：背景停在调试 Pass 的清空色，不画天空 */
			if (render_view->GetDebugViewMode() != DebugViewMode::Wireframe)
				AddSkyPass(*render_view);

			/* 透明物体最后画（天空之后）：调试档位下由调试 Pass 统一重绘，这里跳过，
			 * 免得半透明层盖住调试读数；场景里没有透明物体时整趟都不挂 */
			if (render_view->GetDebugViewMode() == DebugViewMode::None
				&& RenderSorting::HasTransparentObjects(render_view->GetVisibleMeshObjects()))
			{
				AddForwardTransparentPass(*render_view);
			}

			/* 后处理链（效果注册表驱动）：天空之后、输出之前；无效果 / 未启用 = 恒直通 */
			const auto final_output = PostProcess::AddPostProcessPasses(*render_view, lighting_output);
			render_view->SetRenderTargetHandle(final_output);
		}
	}
}
