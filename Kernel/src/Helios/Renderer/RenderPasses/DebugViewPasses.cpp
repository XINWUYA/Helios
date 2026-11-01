#include "Pch.h"
#include "DebugViewPasses.h"
#include "ScenePass.h"
#include <Helios/Core/AssetManager.h>
#include <Helios/Renderer/FrameGraph/FrameGraph.h>
#include <Helios/Renderer/Renderer.h>
#include <Helios/Renderer/RenderView.h>
#include <Helios/Scene/Material.h>
#include <Helios/Scene/Mesh.h>
#include <Helios/Scene/Scene.h>
#include <Helios/Scene/SceneCommon.h>
#include <Helios/VirtualDevice/DeviceShader.h>
#include <Helios/VirtualDevice/DeviceTexture.h>

namespace Helios
{
	namespace DebugView
	{
		namespace
		{
			/* 线框色：模型线框与草的调试重绘共用一份（着色器里的 u_WireframeColor） */
			constexpr glm::vec4 kDebugWireframeColor{ 0.62f, 0.94f, 1.0f, 1.0f };

			/* 几何重绘档位的"主贴图"参数名：与各材质着色器的参数命名同源
			 * （GBuffer / PBRStandard / SimpleLit 都是这些名字）。
			 * Mipmap 档看主贴图（Albedo）；无对应贴图的档位返回空。 */
			const char* MainTextureNameFor(DebugViewMode mode)
			{
				switch (mode)
				{
				case DebugViewMode::Albedo:
				case DebugViewMode::Mipmap:
					return "u_AlbedoTexture";
				case DebugViewMode::Normal:
					return "u_NormalTexture";
				case DebugViewMode::Roughness:
					return "u_RoughnessTexture";
				case DebugViewMode::Metallic:
					return "u_MetallicTexture";
				case DebugViewMode::SpecularColor:
					return "u_SpecularTexture";
				case DebugViewMode::Emission:
					return "u_EmissiveTexture";
				case DebugViewMode::Ambient:
					return "u_AmbientTexture";
				case DebugViewMode::None:
				case DebugViewMode::AmbientOcclusion:
				case DebugViewMode::Diffuse:
				case DebugViewMode::Specular:
				case DebugViewMode::Shadow:
				case DebugViewMode::IndirectDiffuse:
				case DebugViewMode::IndirectSpecular:
				case DebugViewMode::Probe:
				case DebugViewMode::Overdraw:
				case DebugViewMode::Skeleton:
				case DebugViewMode::Wireframe:
					break;
				}
				return nullptr;
			}

			/* 从源材质取一张贴图参数的纹理；参数缺失或类型不符返回空。 */
			SharedPtr<DeviceTexture> FindTextureParam(const SharedPtr<Material>& material, const char* name)
			{
				if (name == nullptr)
					return nullptr;
				const auto& parameters = material->GetAllParameters();
				const auto it = parameters.find(ToID(name));
				if (it == parameters.end() || it->second.Type != ParamType::Texture)
					return nullptr;
				return std::any_cast<std::pair<SharedPtr<DeviceTexture>, uint32_t>>(it->second.Value).first;
			}

			/* 调试材质（几何重绘）：DebugMaterial.glsl + 源材质的主贴图 / UV 变换。
			 * 贴图缺失补白（亮度类缺省值语义）；剔除配置随源材质（双面等姿态一致），
			 * 深度 / 混合用默认值（本 Pass 自带深度，做标准的不透明绘制）。 */
			SharedPtr<Material> CreateDebugMaterial(const SharedPtr<Material>& source,
				const SharedPtr<DeviceShader>& shader, DebugViewMode mode)
			{
				auto material = Material::Create(shader);
				material->SetParameters(ParamType::Int, "u_DebugMode", static_cast<int>(mode));

				SharedPtr<DeviceTexture> main_texture = FindTextureParam(source, MainTextureNameFor(mode));
				material->SetTexture("u_MainTexture", main_texture != nullptr ? main_texture : DeviceTexture::White());

				/* UV 变换与源材质同源（缺省 = 不变换） */
				glm::vec4 tiling_offset(1.0f, 1.0f, 0.0f, 0.0f);
				const auto& parameters = source->GetAllParameters();
				const auto it = parameters.find(ToID("u_AlbedoTilingOffset"));
				if (it != parameters.end() && it->second.Type == ParamType::Vec4)
					tiling_offset = std::any_cast<glm::vec4>(it->second.Value);
				material->SetParameters(ParamType::Vec4, "u_AlbedoTilingOffset", tiling_offset);

				auto& raster_state = material->GetRasterState();
				const auto& source_raster_state = source->GetRasterState();
				raster_state.CullMode = source_raster_state.CullMode;
				raster_state.FrontFaceType = source_raster_state.FrontFaceType;
				return material;
			}

			/* 草的调试材质：一份着色器覆盖 Surface / Mipmap / Overdraw / Wireframe 档。
			 * 底色、粗糙度这些外观参数由每片草地按源材质 per-draw 带过来（见 SubmitGrassDebug），
			 * 一件材质就能服务整场草。Shadow 档另有一份（要采阴影图）。 */
			SharedPtr<Material> CreateGrassDebugMaterial(const SharedPtr<DeviceShader>& shader, DebugViewMode mode)
			{
				auto material = Material::Create(shader);
				material->SetParameters(ParamType::Int, "u_DebugMode", static_cast<int>(mode));
				material->SetParameters(ParamType::Vec4, "u_WireframeColor", kDebugWireframeColor);

				auto& raster_state = material->GetRasterState();
				/* 叶片是单面几何，调试重绘同样不能剔背面 */
				raster_state.CullMode = CullMode::Cull_None;

				if (mode == DebugViewMode::Overdraw)
				{
					raster_state.SetAdditiveBlend();
				}
				else if (mode == DebugViewMode::Wireframe)
				{
					/* 与模型线框同款：不测深度、只画三角形的边 */
					raster_state.EnableDepthTest = false;
					raster_state.EnableDepthWrite = false;
					raster_state.Wireframe = true;
				}
				return material;
			}

			/* Overdraw 计数 Pass：所有不透明对象按"加法混合、不测深度"重画一遍，
			 * 每个片元向计数纹理累加一份步进（DebugMaterial.glsl 的 Overdraw 档）—— 拾取值即
			 * 该像素被写过的次数（饱和），显示层由合成 Pass 的热力图承担。 */
			FrameGraphResourceHandleTyped<FrameGraphTexture> AddOverdrawPass(RenderView& render_view)
			{
				PROFILE_FUNCTION();

				auto& frame_graph = render_view.GetFrameGraph();
				const auto& viewport_region = render_view.GetViewportRegion();

				static BuiltinAsset<DeviceShader> s_material_shader("Shaders/DebugShaders/DebugMaterial.glsl");
				const auto shader = s_material_shader.Get();

				struct OverdrawPassData
				{
					FrameGraphResourceHandleTyped<FrameGraphTexture> Count;
				};

				auto overdraw_pass = frame_graph->AddPass<OverdrawPassData>("DebugOverdrawPass",
					[&](FrameGraphBuilder& builder, OverdrawPassData& data)
					{
						FrameGraphTexture::Descriptor count_desc;
						count_desc.Width = viewport_region.Width;
						count_desc.Height = viewport_region.Height;
						count_desc.TextureFormat = TextureFormat::RGBA8;

						data.Count = builder.CreateTexture("DebugOverdrawCount", count_desc);
						builder.BindOutputResource(data.Count, FrameGraphTexture::Usage::ColorAttachment);

						FrameGraphPassInfo::Descriptor pass_desc;
						pass_desc.Attachments.ColorAttachments(0) = data.Count;
						pass_desc.ViewportRegion = viewport_region;
						builder.CreateRenderPass("DebugOverdrawRenderTarget", pass_desc);
					},
					[&render_view, shader](const FrameGraphResources& resources, const OverdrawPassData&)
					{
						const auto render_pass_info = resources.GetPassRenderTarget();
						render_view.EmplacePassFrameBuffer("DebugOverdraw", render_pass_info);

						render_pass_info->Bind();
						{
							auto material = CreateSharedPtr<Material>();
							auto& raster_state = material->GetRasterState();
							raster_state.SetAdditiveBlend();
							material->SetShader(shader);
							material->SetParameters(ParamType::Int, "u_DebugMode", static_cast<int>(DebugViewMode::Overdraw));
							material->SetTexture("u_MainTexture", DeviceTexture::White());

							for (const auto& mesh_object : render_view.GetVisibleMeshObjects())
							{
								const auto& object_material = mesh_object.Material;
								if (object_material == nullptr || object_material->IsSkyBox())
									continue;

								Renderer::FillObjectUniformBuffer(mesh_object);
								Renderer::Submit(material, mesh_object.MeshSegment->GetMeshPrimitive());
							}

							/* 草不在可见网格列表里：单独补一笔，重叠次数照算 */
							if (const auto scene = render_view.GetOwnerScene())
							{
								static BuiltinAsset<DeviceShader> s_grass_shader("Shaders/DebugShaders/DebugGrass.glsl");
								Forward::SubmitGrassDebug(render_view, *scene,
									CreateGrassDebugMaterial(s_grass_shader.Get(), DebugViewMode::Overdraw));
							}
						}
						render_pass_info->Unbind();
						Renderer::GetRenderAPI()->Flush();
					}
					);

				return overdraw_pass->GetData().Count;
			}

			/* 线框材质：与编辑器模型线框共用一份着色器（只吃位置和一个纯色 uniform）。
			 * 参数全程不变，静态一份逐帧复用 */
			SharedPtr<Material> GetWireframeDebugMaterial()
			{
				static SharedPtr<Material> material;
				if (material != nullptr)
					return material;

				material = CreateSharedPtr<Material>();
				material->SetShader(AssetManager::Instance().GetOrLoad<DeviceShader>(
					ABSOLUTE_PATH("Shaders/Overlay/Wireframe.glsl")));
				material->SetParameters(ParamType::Vec4, "u_WireframeColor", kDebugWireframeColor);

				auto& raster_state = material->GetRasterState();
				raster_state.CullMode = CullMode::Cull_None;
				raster_state.EnableDepthTest = false;
				raster_state.EnableDepthWrite = false;
				raster_state.Wireframe = true;
				return material;
			}

			/* 线框 Pass：所有可见对象用"纯位置 + 纯色"着色器重画一遍，三角形只画边（光栅状态的
			 * Wireframe 位）。不测深度也不写 —— 背面边也看得见，通看结构；
			 * 颜色清空后只落线框（背景停在清空色，纯结构视图）。 */
			void AddWireframePass(RenderView& render_view, const Target& target)
			{
				PROFILE_FUNCTION();

				auto& frame_graph = render_view.GetFrameGraph();
				const auto& viewport_region = render_view.GetViewportRegion();

				struct WireframePassData
				{
					FrameGraphResourceHandleTyped<FrameGraphTexture> Output;
				};

				frame_graph->AddPass<WireframePassData>("DebugWireframePass",
					[&](FrameGraphBuilder& builder, WireframePassData& data)
					{
						data.Output = target.Output;
						builder.BindInputResource(data.Output, FrameGraphTexture::Usage::ColorAttachment);

						FrameGraphPassInfo::Descriptor pass_desc;
						pass_desc.Attachments.ColorAttachments(0) = data.Output;
						pass_desc.ViewportRegion = viewport_region;
						builder.CreateRenderPass("DebugWireframeRenderTarget", pass_desc);
						builder.AsSideEffect();
					},
					[&render_view](const FrameGraphResources& resources, const WireframePassData&)
					{
						const auto render_pass_info = resources.GetPassRenderTarget();
						render_view.EmplacePassFrameBuffer("DebugWireframe", render_pass_info);

						render_pass_info->Bind();
						{
							const SharedPtr<Material> material = GetWireframeDebugMaterial();
							for (const auto& mesh_object : render_view.GetVisibleMeshObjects())
							{
								const auto& object_material = mesh_object.Material;
								if (object_material == nullptr || object_material->IsSkyBox())
									continue;

								Renderer::FillObjectUniformBuffer(mesh_object);
								Renderer::Submit(material, mesh_object.MeshSegment->GetMeshPrimitive());
							}

							/* 草同样要出线框：叶片和块底面按同一套提交补画一遍 */
							if (const auto scene = render_view.GetOwnerScene())
							{
								static BuiltinAsset<DeviceShader> s_grass_shader("Shaders/DebugShaders/DebugGrass.glsl");
								Forward::SubmitGrassDebug(render_view, *scene,
									CreateGrassDebugMaterial(s_grass_shader.Get(), DebugViewMode::Wireframe));
							}
						}
						render_pass_info->Unbind();
						Renderer::GetRenderAPI()->Flush();
					}
					);
			}

			/* Shadow 档的累加 Pass：逐光源把（可见度 × 权重, 权重）加进一张浮点纹理，
			 * 合成端相除得到平均可见度。权重只取"这盏灯够不够得着"，不掺光照公式。 */
			FrameGraphResourceHandleTyped<FrameGraphTexture> AddShadowAccumulatePass(RenderView& render_view)
			{
				PROFILE_FUNCTION();

				auto& frame_graph = render_view.GetFrameGraph();
				const auto& viewport_region = render_view.GetViewportRegion();

				static BuiltinAsset<DeviceShader> s_shadow_shader("Shaders/DebugShaders/DebugShadow.glsl");
				const auto shader = s_shadow_shader.Get();

				struct AccumulatePassData
				{
					FrameGraphResourceHandleTyped<FrameGraphTexture> Accumulate;
					FrameGraphResourceHandleTyped<FrameGraphTexture> ShadowMap;
					FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture0;
					FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture1;
					FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture2;
					FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture3;
					FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture4;
					FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture5;
				};

				auto accumulate_pass = frame_graph->AddPass<AccumulatePassData>("DebugShadowAccumulatePass",
					[&](FrameGraphBuilder& builder, AccumulatePassData& data)
					{
						FrameGraphTexture::Descriptor desc;
						desc.Width = viewport_region.Width;
						desc.Height = viewport_region.Height;
						desc.TextureFormat = TextureFormat::RGBA16F;

						data.Accumulate = builder.CreateTexture("DebugShadowAccumulate", desc);
						data.ShadowMap = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("ShadowMapHandle");

						/* 世界位置 / 法线 / 接受阴影标记都从 G-Buffer 读（与光照阶段同源） */
						auto& blackboard = frame_graph->GetBlackboard();
						data.GBufferTexture0 = blackboard.GetResourceHandle<FrameGraphTexture>("GBufferTexture0");
						data.GBufferTexture1 = blackboard.GetResourceHandle<FrameGraphTexture>("GBufferTexture1");
						data.GBufferTexture2 = blackboard.GetResourceHandle<FrameGraphTexture>("GBufferTexture2");
						data.GBufferTexture3 = blackboard.GetResourceHandle<FrameGraphTexture>("GBufferTexture3");
						data.GBufferTexture4 = blackboard.GetResourceHandle<FrameGraphTexture>("GBufferTexture4");
						data.GBufferTexture5 = blackboard.GetResourceHandle<FrameGraphTexture>("GBufferTexture5");

						builder.BindOutputResource(data.Accumulate, FrameGraphTexture::Usage::ColorAttachment);
						builder.BindInputResource(data.ShadowMap, FrameGraphTexture::Usage::Sampleable);
						builder.BindInputResource(data.GBufferTexture0, FrameGraphTexture::Usage::Sampleable);
						builder.BindInputResource(data.GBufferTexture1, FrameGraphTexture::Usage::Sampleable);
						builder.BindInputResource(data.GBufferTexture2, FrameGraphTexture::Usage::Sampleable);
						builder.BindInputResource(data.GBufferTexture3, FrameGraphTexture::Usage::Sampleable);
						builder.BindInputResource(data.GBufferTexture4, FrameGraphTexture::Usage::Sampleable);
						builder.BindInputResource(data.GBufferTexture5, FrameGraphTexture::Usage::Sampleable);

						FrameGraphPassInfo::Descriptor pass_desc;
						pass_desc.Attachments.ColorAttachments(0) = data.Accumulate;
						pass_desc.ViewportRegion = viewport_region;
						builder.CreateRenderPass("DebugShadowAccumulateRenderTarget", pass_desc);
					},
					[&render_view, shader](const FrameGraphResources& resources, const AccumulatePassData& data)
					{
						const auto render_pass_info = resources.GetPassRenderTarget();
						render_view.EmplacePassFrameBuffer("DebugShadowAccumulate", render_pass_info);

						render_pass_info->Bind();
						{
							Renderer::GetRenderAPI()->Clear();

							/* 阴影图随 Submit 直绑（按 Shader 反射出的 "u_ShadowMap" 绑定点） */
							ScopedShadowMapBinding shadow_map_binding(resources.Get(data.ShadowMap).Texture);

							auto material = CreateSharedPtr<Material>();
							auto& raster_state = material->GetRasterState();
							raster_state.EnableDepthTest = false;
							raster_state.EnableDepthWrite = false;
							raster_state.SetAdditiveBlend();
							material->SetShader(shader);
							material->SetTexture("u_GBufferTexture0", resources.Get(data.GBufferTexture0).Texture);
							material->SetTexture("u_GBufferTexture1", resources.Get(data.GBufferTexture1).Texture);
							material->SetTexture("u_GBufferTexture2", resources.Get(data.GBufferTexture2).Texture);
							material->SetTexture("u_GBufferTexture3", resources.Get(data.GBufferTexture3).Texture);
							material->SetTexture("u_GBufferTexture4", resources.Get(data.GBufferTexture4).Texture);
							material->SetTexture("u_GBufferTexture5", resources.Get(data.GBufferTexture5).Texture);

							/* 每盏灯（有没有阴影数据都算）画一笔，光源参数逐笔重填 */
							for (const auto& light : render_view.GetValidLights())
							{
								Renderer::FillLightUniformBuffer(light, render_view.GetShadowMapManager().get());
								Renderer::Submit(material, Renderer::GetFullScreenVertexArray());
							}
						}
						render_pass_info->Unbind();
						Renderer::GetRenderAPI()->Flush();
					}
					);

				return accumulate_pass->GetData().Accumulate;
			}

			/* 前向的 Shadow 累加 Pass：没有 G-Buffer，就让材质着色器按前向逐光源的老路子再画一遍，
			 * 每笔输出累加编码（首笔覆盖、其余加法叠加），自带深度。天空不画，背景留给合成时保留 */
			FrameGraphResourceHandleTyped<FrameGraphTexture> AddForwardShadowAccumulatePass(RenderView& render_view)
			{
				PROFILE_FUNCTION();

				auto& frame_graph = render_view.GetFrameGraph();
				const auto& viewport_region = render_view.GetViewportRegion();

				struct ForwardShadowPassData
				{
					FrameGraphResourceHandleTyped<FrameGraphTexture> Accumulate;
					FrameGraphResourceHandleTyped<FrameGraphTexture> Depth;
					FrameGraphResourceHandleTyped<FrameGraphTexture> ShadowMap;
				};

				auto accumulate_pass = frame_graph->AddPass<ForwardShadowPassData>("DebugShadowAccumulatePass",
					[&](FrameGraphBuilder& builder, ForwardShadowPassData& data)
					{
						FrameGraphTexture::Descriptor accumulate_desc;
						accumulate_desc.Width = viewport_region.Width;
						accumulate_desc.Height = viewport_region.Height;
						accumulate_desc.TextureFormat = TextureFormat::RGBA16F;

						FrameGraphTexture::Descriptor depth_desc = accumulate_desc;
						depth_desc.TextureFormat = TextureFormat::Depth32;

						data.Accumulate = builder.CreateTexture("DebugShadowAccumulate", accumulate_desc);
						data.Depth = builder.CreateTexture("DebugShadowDepth", depth_desc);
						data.ShadowMap = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("ShadowMapHandle");
						builder.BindOutputResource(data.Accumulate, FrameGraphTexture::Usage::ColorAttachment);
						builder.BindOutputResource(data.Depth, FrameGraphTexture::Usage::DepthAttachment);
						builder.BindInputResource(data.ShadowMap, FrameGraphTexture::Usage::Sampleable);

						FrameGraphPassInfo::Descriptor pass_desc;
						pass_desc.Attachments.ColorAttachments(0) = data.Accumulate;
						pass_desc.Attachments.DepthAttachment() = data.Depth;
						pass_desc.ColorClearValues.resize(1);
						pass_desc.ColorClearValues[0] = glm::vec4(0.0f);
						pass_desc.ViewportRegion = viewport_region;
						builder.CreateRenderPass("DebugShadowAccumulateRenderTarget", pass_desc);
					},
					[&render_view](const FrameGraphResources& resources, const ForwardShadowPassData& data)
					{
						const auto scene = render_view.GetOwnerScene();
						if (scene == nullptr)
							return;

						const auto render_pass_info = resources.GetPassRenderTarget();
						render_view.EmplacePassFrameBuffer("DebugShadowAccumulate", render_pass_info);

						render_pass_info->Bind();
						{
							Renderer::GetRenderAPI()->Clear();
							ScopedShadowMapBinding shadow_map_binding(resources.Get(data.ShadowMap).Texture);
							Forward::SubmitSceneObjects(render_view, *scene, false, Forward::SubmitBatch::All,
								DebugViewMode::Shadow);
							Forward::SubmitGrassFields(render_view, *scene, DebugViewMode::Shadow);
						}
						render_pass_info->Unbind();
						Renderer::GetRenderAPI()->Flush();
					}
					);

				return accumulate_pass->GetData().Accumulate;
			}

			/* 调试合成 Pass：全屏把 Surface 档位（延迟 = G-Buffer 直读）、Shadow 档的平均可见度
			 * 或 Overdraw 热力图覆写进视图输出（延迟管线背景由天空 Pass 按深度收尾；前向的 Shadow 档
			 * 只覆写有几何的像素，天空留在场景 Pass 画好的底上）。采样槽位没来源时由 CPU 补黑。 */
			void AddCompositePass(RenderView& render_view, const Target& target, DebugViewMode mode,
				FrameGraphResourceHandleTyped<FrameGraphTexture> overdraw_count,
				FrameGraphResourceHandleTyped<FrameGraphTexture> shadow_accumulate)
			{
				PROFILE_FUNCTION();

				auto& frame_graph = render_view.GetFrameGraph();
				const auto& viewport_region = render_view.GetViewportRegion();

				static BuiltinAsset<DeviceShader> s_composite_shader("Shaders/DebugShaders/DebugView.glsl");
				const auto shader = s_composite_shader.Get();

				struct CompositePassData
				{
					FrameGraphResourceHandleTyped<FrameGraphTexture> Output;
					FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture0;
					FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture1;
					FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture2;
					FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture3;
					FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture4;
					FrameGraphResourceHandleTyped<FrameGraphTexture> GBufferTexture5;
					FrameGraphResourceHandleTyped<FrameGraphTexture> OverdrawCount;
					FrameGraphResourceHandleTyped<FrameGraphTexture> ShadowAccumulate;
				};

				frame_graph->AddPass<CompositePassData>("DebugViewCompositePass",
					[&, mode, overdraw_count, shadow_accumulate](FrameGraphBuilder& builder, CompositePassData& data)
					{
						data.Output = target.Output;
						builder.BindInputResource(data.Output, FrameGraphTexture::Usage::ColorAttachment);

						if (target.HasGBuffer)
						{
							auto& blackboard = frame_graph->GetBlackboard();
							data.GBufferTexture0 = blackboard.GetResourceHandle<FrameGraphTexture>("GBufferTexture0");
							data.GBufferTexture1 = blackboard.GetResourceHandle<FrameGraphTexture>("GBufferTexture1");
							data.GBufferTexture2 = blackboard.GetResourceHandle<FrameGraphTexture>("GBufferTexture2");
							data.GBufferTexture3 = blackboard.GetResourceHandle<FrameGraphTexture>("GBufferTexture3");
							data.GBufferTexture4 = blackboard.GetResourceHandle<FrameGraphTexture>("GBufferTexture4");
							data.GBufferTexture5 = blackboard.GetResourceHandle<FrameGraphTexture>("GBufferTexture5");
							builder.BindInputResource(data.GBufferTexture0, FrameGraphTexture::Usage::Sampleable);
							builder.BindInputResource(data.GBufferTexture1, FrameGraphTexture::Usage::Sampleable);
							builder.BindInputResource(data.GBufferTexture2, FrameGraphTexture::Usage::Sampleable);
							builder.BindInputResource(data.GBufferTexture3, FrameGraphTexture::Usage::Sampleable);
							builder.BindInputResource(data.GBufferTexture4, FrameGraphTexture::Usage::Sampleable);
							builder.BindInputResource(data.GBufferTexture5, FrameGraphTexture::Usage::Sampleable);
						}

						/* Overdraw 档读计数纹理、Shadow 档读累加纹理（不绑定会被资源生命周期提前释放） */
						data.OverdrawCount = overdraw_count;
						if (mode == DebugViewMode::Overdraw)
							builder.BindInputResource(data.OverdrawCount, FrameGraphTexture::Usage::Sampleable);
						data.ShadowAccumulate = shadow_accumulate;
						if (mode == DebugViewMode::Shadow)
							builder.BindInputResource(data.ShadowAccumulate, FrameGraphTexture::Usage::Sampleable);

						FrameGraphPassInfo::Descriptor pass_desc;
						pass_desc.Attachments.ColorAttachments(0) = data.Output;
						/* 前向保留背景：载入场景 Pass 画好的天空，跳过的像素才看得到它 */
						pass_desc.PreserveContent = !target.HasGBuffer;
						pass_desc.ViewportRegion = viewport_region;
						builder.CreateRenderPass("DebugViewRenderTarget", pass_desc);
						builder.AsSideEffect();
					},
					[&render_view, shader, mode, preserve_background = !target.HasGBuffer](
						const FrameGraphResources& resources, const CompositePassData& data)
					{
						const auto render_pass_info = resources.GetPassRenderTarget();
						render_view.EmplacePassFrameBuffer("DebugView", render_pass_info);

						render_pass_info->Bind();
						{
							const auto black = DeviceTexture::Black();

							auto material = CreateSharedPtr<Material>();
							material->SetShader(shader);
							material->SetParameters(ParamType::Int, "u_DebugMode", static_cast<int>(mode));
							material->SetParameters(ParamType::Int, "u_PreserveBackground", preserve_background ? 1 : 0);
							material->SetTexture("u_GBufferTexture0", data.GBufferTexture0 ? resources.Get(data.GBufferTexture0).Texture : black);
							material->SetTexture("u_GBufferTexture1", data.GBufferTexture1 ? resources.Get(data.GBufferTexture1).Texture : black);
							material->SetTexture("u_GBufferTexture2", data.GBufferTexture2 ? resources.Get(data.GBufferTexture2).Texture : black);
							material->SetTexture("u_GBufferTexture3", data.GBufferTexture3 ? resources.Get(data.GBufferTexture3).Texture : black);
							material->SetTexture("u_GBufferTexture4", data.GBufferTexture4 ? resources.Get(data.GBufferTexture4).Texture : black);
							material->SetTexture("u_GBufferTexture5", data.GBufferTexture5 ? resources.Get(data.GBufferTexture5).Texture : black);
							material->SetTexture("u_OverdrawTexture", data.OverdrawCount ? resources.Get(data.OverdrawCount).Texture : black);
							material->SetTexture("u_ShadowAccumTexture", data.ShadowAccumulate ? resources.Get(data.ShadowAccumulate).Texture : black);

							Renderer::Submit(material, Renderer::GetFullScreenVertexArray());
						}
						render_pass_info->Unbind();
					}
					);
			}

			/* 几何重绘 Pass：逐对象用调试材质重画一遍（Mipmap 档）。自带深度附件
			 * （清空 + 正常比较）—— 遮挡在本 Pass 内部解决，不依赖基准管线的深度；颜色附件也清空
			 * （背景交给天空 / 清屏色）。 */
			void AddDebugMaterialPass(RenderView& render_view, const Target& target, DebugViewMode mode)
			{
				PROFILE_FUNCTION();

				auto& frame_graph = render_view.GetFrameGraph();
				const auto& viewport_region = render_view.GetViewportRegion();

				static BuiltinAsset<DeviceShader> s_material_shader("Shaders/DebugShaders/DebugMaterial.glsl");
				const auto shader = s_material_shader.Get();

				struct DebugMaterialPassData
				{
					FrameGraphResourceHandleTyped<FrameGraphTexture> Output;
					FrameGraphResourceHandleTyped<FrameGraphTexture> Depth;
				};

				frame_graph->AddPass<DebugMaterialPassData>("DebugMaterialPass",
					[&](FrameGraphBuilder& builder, DebugMaterialPassData& data)
					{
						FrameGraphTexture::Descriptor depth_desc;
						depth_desc.Width = viewport_region.Width;
						depth_desc.Height = viewport_region.Height;
						depth_desc.TextureFormat = TextureFormat::Depth32;

						data.Output = target.Output;
						data.Depth = builder.CreateTexture("DebugMaterialDepth", depth_desc);
						builder.BindInputResource(data.Output, FrameGraphTexture::Usage::ColorAttachment);
						builder.BindOutputResource(data.Depth, FrameGraphTexture::Usage::DepthAttachment);

						FrameGraphPassInfo::Descriptor pass_desc;
						pass_desc.Attachments.ColorAttachments(0) = data.Output;
						pass_desc.Attachments.DepthAttachment() = data.Depth;
						pass_desc.ViewportRegion = viewport_region;
						builder.CreateRenderPass("DebugMaterialRenderTarget", pass_desc);
						builder.AsSideEffect();
					},
					[&render_view, shader, mode](const FrameGraphResources& resources, const DebugMaterialPassData&)
					{
						const auto render_pass_info = resources.GetPassRenderTarget();
						render_view.EmplacePassFrameBuffer("DebugMaterial", render_pass_info);

						render_pass_info->Bind();
						{
							/* 调试材质按源材质缓存：同材质多对象复用一份绑定 */
							std::unordered_map<const Material*, SharedPtr<Material>> debug_materials;
							for (const auto& mesh_object : render_view.GetVisibleMeshObjects())
							{
								const auto& material = mesh_object.Material;
								if (material == nullptr || material->IsSkyBox())
									continue;

								auto [it, inserted] = debug_materials.try_emplace(material.get());
								if (inserted)
									it->second = CreateDebugMaterial(material, shader, mode);

								Renderer::FillObjectUniformBuffer(mesh_object);
								Renderer::Submit(it->second, mesh_object.MeshSegment->GetMeshPrimitive());
							}

							/* 草跟不上面的重绘列表（它不在可见网格里），单独走一趟 */
							if (const auto scene = render_view.GetOwnerScene())
							{
								static BuiltinAsset<DeviceShader> s_grass_shader("Shaders/DebugShaders/DebugGrass.glsl");
								Forward::SubmitGrassDebug(render_view, *scene,
									CreateGrassDebugMaterial(s_grass_shader.Get(), mode));
							}
						}
						render_pass_info->Unbind();
						Renderer::GetRenderAPI()->Flush();
					}
					);
			}
		}

		/* 合成之后的草：Surface / Shadow 档由合成 Pass 整屏覆写，草（前向直画、没有 G-Buffer 数据）
		 * 只能在这之后补一笔。深度沿用 G-Buffer，被场景几何挡住的草照常剔除。 */
		void AddGrassOverlayPass(RenderView& render_view, const Target& target, DebugViewMode mode)
		{
			PROFILE_FUNCTION();

			const auto scene = render_view.GetOwnerScene();
			if (scene == nullptr || !Forward::HasVisibleGrassFields(*scene, render_view.GetCullingMask()))
				return;

			auto& frame_graph = render_view.GetFrameGraph();
			const auto& viewport_region = render_view.GetViewportRegion();
			const bool shadow_mode = mode == DebugViewMode::Shadow;

			struct GrassOverlayPassData
			{
				FrameGraphResourceHandleTyped<FrameGraphTexture> Output;
				FrameGraphResourceHandleTyped<FrameGraphTexture> Depth;
				FrameGraphResourceHandleTyped<FrameGraphTexture> ShadowMap;
			};

			frame_graph->AddPass<GrassOverlayPassData>("DebugGrassPass",
				[&, shadow_mode](FrameGraphBuilder& builder, GrassOverlayPassData& data)
				{
					auto& blackboard = frame_graph->GetBlackboard();
					data.Output = target.Output;
					data.Depth = blackboard.GetResourceHandle<FrameGraphTexture>("GBufferDepth");
					if (shadow_mode)
						data.ShadowMap = blackboard.GetResourceHandle<FrameGraphTexture>("ShadowMapHandle");

					builder.BindInputResource(data.Output, FrameGraphTexture::Usage::ColorAttachment);
					builder.BindInputResource(data.Depth, FrameGraphTexture::Usage::DepthAttachment);
					if (shadow_mode)
						builder.BindInputResource(data.ShadowMap, FrameGraphTexture::Usage::Sampleable);

					FrameGraphPassInfo::Descriptor pass_desc;
					pass_desc.Attachments.ColorAttachments(0) = data.Output;
					pass_desc.Attachments.DepthAttachment() = data.Depth;
					/* 只往合成结果上落笔：颜色与场景深度都保留 */
					pass_desc.PreserveContent = true;
					pass_desc.ViewportRegion = viewport_region;
					builder.CreateRenderPass("DebugGrassRenderTarget", pass_desc);
					builder.AsSideEffect();
				},
				[&render_view, shadow_mode](const FrameGraphResources& resources, const GrassOverlayPassData& data)
				{
					const auto scene = render_view.GetOwnerScene();
					if (scene == nullptr)
						return;

					const auto render_pass_info = resources.GetPassRenderTarget();
					render_view.EmplacePassFrameBuffer("DebugGrass", render_pass_info);

					render_pass_info->Bind();
					{
						if (shadow_mode)
						{
							/* 可见度读数要阴影图：光照 UBO 按主方向光填，跟可见着色同一份 */
							static BuiltinAsset<DeviceShader> s_shadow_shader("Shaders/DebugShaders/DebugGrassShadow.glsl");
							ScopedShadowMapBinding shadow_map_binding(resources.Get(data.ShadowMap).Texture);
							Forward::FillGrassLightUniform(render_view);
							Forward::SubmitGrassDebug(render_view, *scene,
								CreateGrassDebugMaterial(s_shadow_shader.Get(), DebugViewMode::Shadow));
						}
						else
						{
							static BuiltinAsset<DeviceShader> s_grass_shader("Shaders/DebugShaders/DebugGrass.glsl");
							Forward::SubmitGrassDebug(render_view, *scene,
								CreateGrassDebugMaterial(s_grass_shader.Get(), render_view.GetDebugViewMode()));
						}
					}
					render_pass_info->Unbind();
					Renderer::GetRenderAPI()->Flush();
				}
				);
		}

		DebugViewMode GetForwardMaterialMode(DebugViewMode mode)
		{
			switch (mode)
			{
			case DebugViewMode::Albedo:
			case DebugViewMode::Normal:
			case DebugViewMode::Roughness:
			case DebugViewMode::Metallic:
			case DebugViewMode::SpecularColor:
			case DebugViewMode::AmbientOcclusion:
			case DebugViewMode::Emission:
			case DebugViewMode::Ambient:
			case DebugViewMode::Diffuse:
			case DebugViewMode::Specular:
			case DebugViewMode::IndirectDiffuse:
			case DebugViewMode::IndirectSpecular:
			case DebugViewMode::Probe:
				return mode;
			case DebugViewMode::None:
			case DebugViewMode::Shadow:
			case DebugViewMode::Overdraw:
			case DebugViewMode::Mipmap:
			case DebugViewMode::Skeleton:
			case DebugViewMode::Wireframe:
				break;
			}
			return DebugViewMode::None;
		}

		SharedPtr<Material> CreateFallbackMaterial(const SharedPtr<Material>& source, DebugViewMode mode)
		{
			static BuiltinAsset<DeviceShader> s_material_shader("Shaders/DebugShaders/DebugMaterial.glsl");
			return CreateDebugMaterial(source, s_material_shader.Get(), mode);
		}

		void AddDebugViewPasses(RenderView& render_view, const Target& target)
		{
			PROFILE_FUNCTION();

			const DebugViewMode mode = render_view.GetDebugViewMode();

			switch (mode)
			{
			case DebugViewMode::None:
				return;

			/* Lighting 类：延迟在光照阶段分流（DebugShaders/DebugLighting.glsl），
			 * 前向由材质着色器在场景 Pass 里输出（DebugShaders/DebugForward.glsl），都不用额外 Pass */
			case DebugViewMode::Diffuse:
			case DebugViewMode::Specular:
			case DebugViewMode::IndirectDiffuse:
			case DebugViewMode::IndirectSpecular:
			case DebugViewMode::Probe:
				return;

			/* Shadow：多光源叠加，逐光源累加后合成。延迟从 G-Buffer 全屏累加，草在合成后补；
			 * 前向让材质着色器重画一遍累加（草也在里面） */
			case DebugViewMode::Shadow:
			{
				if (!target.HasGBuffer)
				{
					AddCompositePass(render_view, target, mode, {}, AddForwardShadowAccumulatePass(render_view));
					return;
				}

				const auto accumulate = AddShadowAccumulatePass(render_view);
				AddCompositePass(render_view, target, mode, {}, accumulate);
				AddGrassOverlayPass(render_view, target, mode);
				return;
			}

			/* 骨骼节点预览：叠加层由编辑器视口的 gizmo 画（见 SceneGizmos），管线本体不变 */
			case DebugViewMode::Skeleton:
				return;

			case DebugViewMode::Overdraw:
			{
				const auto count = AddOverdrawPass(render_view);
				AddCompositePass(render_view, target, mode, count, {});
				return;
			}

			case DebugViewMode::Mipmap:
				AddDebugMaterialPass(render_view, target, mode);
				return;

			/* 线框：纯色画三角形的边，两管线同一套几何重绘 */
			case DebugViewMode::Wireframe:
				AddWireframePass(render_view, target);
				return;

			/* Surface 类：延迟直读 G-Buffer（合成）；前向由材质着色器在场景 Pass 里输出 */
			case DebugViewMode::Albedo:
			case DebugViewMode::Normal:
			case DebugViewMode::Roughness:
			case DebugViewMode::Metallic:
			case DebugViewMode::SpecularColor:
			case DebugViewMode::AmbientOcclusion:
			case DebugViewMode::Emission:
			case DebugViewMode::Ambient:
				if (target.HasGBuffer)
				{
					AddCompositePass(render_view, target, mode, {}, {});
					AddGrassOverlayPass(render_view, target, mode);
				}
				return;
			}
		}
	}
}
