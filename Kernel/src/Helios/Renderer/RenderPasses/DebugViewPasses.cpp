#include "Pch.h"
#include "DebugViewPasses.h"
#include <Helios/Application/AssetManager.h>
#include <Helios/Renderer/FrameGraph/FrameGraph.h>
#include <Helios/Renderer/Renderer.h>
#include <Helios/Renderer/RenderView.h>
#include <Helios/Scene/Material.h>
#include <Helios/Scene/Mesh.h>
#include <Helios/Scene/SceneCommon.h>
#include <Helios/VirtualDevice/DeviceShader.h>
#include <Helios/VirtualDevice/DeviceTexture.h>

namespace Helios
{
	namespace DebugView
	{
		namespace
		{
			/* 几何重绘档位的"主贴图"参数名：与各材质着色器的参数命名同源
			 * （GBufferMaterial / PBRStandard / BuiltinLit 都是这些名字）。
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
				case DebugViewMode::Indirect:
				case DebugViewMode::Overdraw:
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

			/* Overdraw 计数 Pass：所有不透明对象按"加法混合、不测深度"重画一遍，
			 * 每个片元向计数纹理累加 DebugOverdraw.glsl 里的步进值 —— 拾取值即
			 * 该像素被写过的次数（饱和），显示层由合成 Pass 的热力图承担。 */
			FrameGraphResourceHandleTyped<FrameGraphTexture> AddOverdrawPass(RenderView& render_view)
			{
				PROFILE_FUNCTION();

				auto& frame_graph = render_view.GetFrameGraph();
				const auto& viewport_region = render_view.GetViewportRegion();

				const auto shader = ShaderAssetManager::Instance().GetOrLoad(
					ABSOLUTE_PATH("Shaders/DebugShaders/DebugOverdraw.glsl"));

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
							raster_state.EnableBlend = true;
							raster_state.BlendEquationRGB = BlendEquation::Add;
							raster_state.BlendEquationA = BlendEquation::Add;
							raster_state.BlendFuncSrcRGB = BlendFunc::One;
							raster_state.BlendFuncSrcA = BlendFunc::One;
							raster_state.BlendFuncDstRGB = BlendFunc::One;
							raster_state.BlendFuncDstA = BlendFunc::One;
							material->SetShader(shader);

							for (const auto& mesh_object : render_view.GetVisibleMeshObjects())
							{
								const auto& object_material = mesh_object.Material;
								if (object_material == nullptr || object_material->IsSkyBox())
									continue;

								Renderer::FillObjectUniformBuffer(mesh_object);
								Renderer::Submit(material, mesh_object.MeshSegment->GetMeshPrimitive());
							}
						}
						render_pass_info->Unbind();
						Renderer::GetRenderAPI()->Flush();
					}
					);

				return overdraw_pass->GetData().Count;
			}

			/* 调试合成 Pass：全屏把 Surface 档位（延迟 = G-Buffer 直读）或 Overdraw 热力图覆写进视图
			 * 输出（延迟管线背景由天空 Pass 按深度收尾；前向背景就是清屏色）。采样槽位没来源时由 CPU
			 * 补黑（声明过的采样器每次绘制必须有绑定）。 */
			void AddCompositePass(RenderView& render_view, const Target& target, DebugViewMode mode,
				FrameGraphResourceHandleTyped<FrameGraphTexture> overdraw_count)
			{
				PROFILE_FUNCTION();

				auto& frame_graph = render_view.GetFrameGraph();
				const auto& viewport_region = render_view.GetViewportRegion();

				const auto shader = ShaderAssetManager::Instance().GetOrLoad(
					ABSOLUTE_PATH("Shaders/DebugShaders/DebugView.glsl"));

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
				};

				frame_graph->AddPass<CompositePassData>("DebugViewCompositePass",
					[&, mode, overdraw_count](FrameGraphBuilder& builder, CompositePassData& data)
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

						/* Overdraw 档读计数纹理（不绑定会被资源生命周期提前释放） */
						data.OverdrawCount = overdraw_count;
						if (mode == DebugViewMode::Overdraw)
							builder.BindInputResource(data.OverdrawCount, FrameGraphTexture::Usage::Sampleable);

						FrameGraphPassInfo::Descriptor pass_desc;
						pass_desc.Attachments.ColorAttachments(0) = data.Output;
						pass_desc.ViewportRegion = viewport_region;
						builder.CreateRenderPass("DebugViewRenderTarget", pass_desc);
						builder.AsSideEffect();
					},
					[&render_view, shader, mode](const FrameGraphResources& resources, const CompositePassData& data)
					{
						const auto render_pass_info = resources.GetPassRenderTarget();
						render_view.EmplacePassFrameBuffer("DebugView", render_pass_info);

						render_pass_info->Bind();
						{
							const auto black = DeviceTexture::Black();

							auto material = CreateSharedPtr<Material>();
							material->SetShader(shader);
							material->SetParameters(ParamType::Int, "u_DebugMode", static_cast<int>(mode));
							material->SetTexture("u_GBufferTexture0", data.GBufferTexture0 ? resources.Get(data.GBufferTexture0).Texture : black);
							material->SetTexture("u_GBufferTexture1", data.GBufferTexture1 ? resources.Get(data.GBufferTexture1).Texture : black);
							material->SetTexture("u_GBufferTexture2", data.GBufferTexture2 ? resources.Get(data.GBufferTexture2).Texture : black);
							material->SetTexture("u_GBufferTexture3", data.GBufferTexture3 ? resources.Get(data.GBufferTexture3).Texture : black);
							material->SetTexture("u_GBufferTexture4", data.GBufferTexture4 ? resources.Get(data.GBufferTexture4).Texture : black);
							material->SetTexture("u_GBufferTexture5", data.GBufferTexture5 ? resources.Get(data.GBufferTexture5).Texture : black);
							material->SetTexture("u_OverdrawTexture", data.OverdrawCount ? resources.Get(data.OverdrawCount).Texture : black);

							Renderer::Submit(material, Renderer::GetFullScreenVertexArray());
						}
						render_pass_info->Unbind();
					}
					);
			}

			/* 几何重绘 Pass：逐对象用调试材质重画一遍（Mipmap 档和前向 Surface 档）。自带深度附件
			 * （清空 + 正常比较）—— 遮挡在本 Pass 内部解决，不依赖基准管线的深度；颜色附件也清空
			 * （背景交给天空 / 清屏色）。 */
			void AddDebugMaterialPass(RenderView& render_view, const Target& target, DebugViewMode mode)
			{
				PROFILE_FUNCTION();

				auto& frame_graph = render_view.GetFrameGraph();
				const auto& viewport_region = render_view.GetViewportRegion();

				const auto shader = ShaderAssetManager::Instance().GetOrLoad(
					ABSOLUTE_PATH("Shaders/DebugShaders/DebugMaterial.glsl"));

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
						}
						render_pass_info->Unbind();
						Renderer::GetRenderAPI()->Flush();
					}
					);
			}
		}

		void AddDebugViewPasses(RenderView& render_view, const Target& target)
		{
			PROFILE_FUNCTION();

			const DebugViewMode mode = render_view.GetDebugViewMode();

			switch (mode)
			{
			case DebugViewMode::None:
				return;

			/* Lighting 类：在光照阶段分流（见 DebugShaders/DebugLighting.glsl
			 * 的 DebugLightingCompose），无需额外 Pass */
			case DebugViewMode::Diffuse:
			case DebugViewMode::Specular:
			case DebugViewMode::Shadow:
			case DebugViewMode::Indirect:
				return;

			case DebugViewMode::Overdraw:
			{
				const auto count = AddOverdrawPass(render_view);
				AddCompositePass(render_view, target, mode, count);
				return;
			}

			case DebugViewMode::Mipmap:
				AddDebugMaterialPass(render_view, target, mode);
				return;

			/* Surface 类：延迟直读 G-Buffer（合成）；前向无 G-Buffer，材质重采样近似 */
			case DebugViewMode::Albedo:
			case DebugViewMode::Normal:
			case DebugViewMode::Roughness:
			case DebugViewMode::Metallic:
			case DebugViewMode::SpecularColor:
			case DebugViewMode::AmbientOcclusion:
			case DebugViewMode::Emission:
			case DebugViewMode::Ambient:
				if (target.HasGBuffer)
					AddCompositePass(render_view, target, mode, {});
				else
					AddDebugMaterialPass(render_view, target, mode);
				return;
			}
		}
	}
}
