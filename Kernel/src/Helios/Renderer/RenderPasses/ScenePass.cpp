#include "Pch.h"
#include "ScenePass.h"
#include "DebugViewPasses.h"
#include "GrassDrawParams.h"
#include "ObjectSorting.h"
#include "TerrainPasses.h"
#include <Helios/Core/AssetManager.h>
#include <Helios/Renderer/FrameGraph/FrameGraph.h>
#include <Helios/Renderer/PostProcess/PostProcessPasses.h>
#include <Helios/Renderer/RenderView.h>
#include <Helios/Profiler/RenderStatistics.h>
#include <Helios/Renderer/Renderer.h>
#include <Helios/Scene/Camera.h>
#include <Helios/Scene/Components.h>
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
				state.SetAdditiveBlend();
				state.DepthCompareFunc = CompareFunc::Equal;
				return state;
			}

			/* 前向绘制公共体：按渲染队列分两批提交（不透明先、透明后），每批内逐光源一笔合成 ——
			 * 第 0 笔完整着色（直接光 + 环境 / 自发光，用材质自身的光栅状态），其余笔加法混合 +
			 * Equal 比较，只输出该光源的直接光。天空盒和 Unlit 单笔原样提交；
			 * 调用方自己负责绑目标、视口和阴影纹理。 */
			void SubmitForwardObjects(RenderView& render_view, Scene& scene, bool transparent,
				const SubmitParams& params)
			{
				const auto probe_manager = params.IgnoreProbes ? nullptr : scene.GetReflectionProbeManager();
				const auto& lights = render_view.GetValidLights();
				const ShadowMapManager* shadow_maps = render_view.GetShadowMapManager().get();
				const glm::vec3 view_position = (params.SortOrigin != nullptr)
					? *params.SortOrigin
					: ((render_view.GetCullingCamera() != nullptr)
						? render_view.GetCullingCamera()->GetPosition() : glm::vec3(0.0f));

				const auto& visible_objects = (params.Objects != nullptr)
					? *params.Objects : render_view.GetVisibleMeshObjects();
				const auto draw_list = (params.Batch == SubmitBatch::All)
					? RenderSorting::CollectDrawList(visible_objects, transparent, view_position)
					: RenderSorting::CollectDrawList(visible_objects, transparent, view_position,
						[](const VisibleMeshObject& object) { return RenderSorting::IsForwardOnlyOpaque(object); });

				/* 环境项放在主阴影光源那一笔里合成：只有那笔的阴影因子非 0，
				 * 环境项才吃得到阴影（见 AmbientShadowVisibility） */
				const size_t ambient_light_index = (shadow_maps != nullptr)
					? shadow_maps->FindPrimaryShadowLightIndex(lights) : 0;

				/* 前向直画用材质自身 shader：没写的贴图槽补默认贴图（缺绑定 Metal 会断言），
				 * 没写的值参数补默认值（裸 uniform 是 per-program 状态，不补会沿用上一个材质的值）。
				 * per_draw 跨对象复用，覆盖表只清不释放 */
				DrawParams per_draw;
				per_draw.FillMissing = MaterialFill::Values | MaterialFill::Textures;
				per_draw.Overrides.reserve(8);

				/* 调试档位：Surface 档只出一次读数，不走逐光源；接不上调试分流的材质按源材质换调试材质。
				 * Probe 档的颜色序号按延迟同一张表取（离相机最近的三个探针），两条管线颜色对得上 */
				const DebugViewMode debug_view = params.DebugView;
				const bool debug = debug_view != DebugViewMode::None;
				const bool debug_single_pass = debug_view >= DebugViewMode::Albedo && debug_view <= DebugViewMode::Ambient;
				std::unordered_map<const Material*, SharedPtr<Material>> fallback_materials;
				std::vector<SharedPtr<ReflectionProbe>> debug_probes;
				if (debug_view == DebugViewMode::Probe && probe_manager != nullptr)
					debug_probes = probe_manager->CollectClosestBakedProbes(view_position, 3, render_view.GetCullingMask());

				for (const VisibleMeshObject* mesh_object : draw_list)
				{
					Renderer::FillObjectUniformBuffer(*mesh_object);
					const auto& material = mesh_object->Material;

					if (debug && material->IsSkyBox() && debug_view == DebugViewMode::Shadow)
						continue;
					if (debug && !material->IsSkyBox() && !material->SupportsDebugView())
					{
						auto [it, inserted] = fallback_materials.try_emplace(material.get());
						if (inserted)
							it->second = DebugView::CreateFallbackMaterial(material, debug_view);
						Renderer::Submit(it->second, mesh_object->MeshSegment->GetMeshPrimitive());
						continue;
					}

					const RenderRasterState base_state = transparent
						? RenderSorting::MakeTransparentRasterState(material->GetRasterState())
						: material->GetRasterState();

					/* IBL 是"每次绘制都不一样"的绑定（按对象选最近探针）—— 走 per-draw 覆盖、不写进材质对象；
					 * 只对支持 IBL 的 Shader 发；没有探针时靠覆盖里的中性兜底，保证四个采样槽始终有绑定。 */
					per_draw.Overrides.clear();
					per_draw.RasterStateOverride.reset();
					if (transparent)
						per_draw.RasterStateOverride = base_state;

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

						if (debug_view == DebugViewMode::Probe)
						{
							const auto found = std::find(debug_probes.begin(), debug_probes.end(), chosen);
							const int index = found != debug_probes.end()
								? static_cast<int>(found - debug_probes.begin()) : -1;
							per_draw.Overrides.emplace_back(ParamType::Int, "u_DebugProbeIndex", index);
						}
					}

					if (debug && material->SupportsDebugView())
						per_draw.Overrides.emplace_back(ParamType::Int, "u_DebugView", static_cast<int>(debug_view));

					const auto& mesh_primitive = mesh_object->MeshSegment->GetMeshPrimitive();

					/* 天空盒与不参与光照合成的材质：单笔原样提交 */
					if (material->IsSkyBox() || !material->SupportsDirectLighting())
					{
						Renderer::Submit(material, mesh_primitive, &per_draw);
						continue;
					}

					/* 合成开关放在覆盖表最后，逐笔只改它的值 */
					per_draw.Overrides.emplace_back(ParamType::Int, "u_ComposeAmbientEmission", 1);
					std::any& compose_value = per_draw.Overrides.back().Value;

					/* 本视图没有光源（都落在看不到的层）：仍画一笔环境光 + IBL，
					 * 否则物体整片消失 */
					if (lights.empty())
					{
						Renderer::FillEmptyLightUniformBuffer();
						Renderer::Submit(material, mesh_primitive, &per_draw);
						continue;
					}

					if (debug_single_pass)
					{
						Renderer::FillLightUniformBuffer(lights[std::min(ambient_light_index, lights.size() - 1)], shadow_maps);
						Renderer::Submit(material, mesh_primitive, &per_draw);
						continue;
					}

					for (size_t light_index = 0; light_index < lights.size(); ++light_index)
					{
						Renderer::FillLightUniformBuffer(lights[light_index], shadow_maps);

						/* 合成开关只在环境项那一笔打开，其余笔加法叠加 */
						compose_value = light_index == ambient_light_index ? 1 : 0;
						if (light_index > 0)
							per_draw.RasterStateOverride = MakeAdditiveRasterState(base_state);

						Renderer::Submit(material, mesh_primitive, &per_draw);
					}
				}
			}

			/* 逐片草地的绘制：块底面先、叶片后（抽稀 / 剔除 / 间接），材质与逐绘制参数
			 * 由调用方准备 —— 可见着色与调试重绘共用这一套提交。 */
			template <typename GetMaterial, typename PrepareDraw>
			void SubmitGrassDraws(RenderView& render_view, Scene& scene,
				GetMaterial&& get_material, PrepareDraw&& prepare_draw)
			{
				const Camera* camera = render_view.GetCullingCamera();
				if (camera == nullptr)
					return;

				const Frustum frustum = Frustum::FromViewProjection(camera->GetViewProjectionMatrix());
				const bool cull = render_view.IsEnableFrustumCulling();
				const glm::vec3 view_position = camera->GetPosition();
				const MeshPrimitive& blade = GrassField::GetBladePrimitive();

				for (auto [entity, grass] : scene.GetRegistry().view<GrassComponent>().each())
				{
					if (grass.GetMaterial() == nullptr || grass.m_Field == nullptr || grass.m_Field->IsEmpty())
						continue;
					if (!scene.IsEntityInRenderView(entity, render_view.GetCullingMask()))
						continue;

					const auto& instance_buffer = grass.m_Field->GetInstanceBuffer();
					if (instance_buffer == nullptr)
						continue;

					const SharedPtr<Material> material = get_material(grass);
					if (material == nullptr)
						continue;

					/* 风场、脚印走 per-draw 覆盖（跟阴影投影共用同一份参数），
					 * 之后再把「叶片 / 块底面」这组开关接上 */
					DrawParams per_draw = GrassDraw::MakeDrawParams(grass);
					/* 叶片与块底面共用一个材质，靠这个开关分开画。
					 * 调用方后面还会追加覆盖，所以这里记下标，不能用 back() */
					const size_t flat_pad_index = per_draw.Overrides.size();
					per_draw.Overrides.emplace_back(ParamType::Int, "u_FlatPad", 0);
					/* 叶片停画之后草色再撑一段，交接不至于突然断掉 */
					per_draw.Overrides.emplace_back(ParamType::Float, "u_PadCullDistance",
						grass.m_EndCullDistance * 2.0f);
					/* 抽稀档位与风场关停距离：与 GrassComponent 同名同义 */
					per_draw.Overrides.emplace_back(ParamType::Float, "u_BladeFullDistance",
						grass.m_StartCullDistance);
					per_draw.Overrides.emplace_back(ParamType::Float, "u_BladeFadeDistance",
						grass.m_EndCullDistance);
					per_draw.Overrides.emplace_back(ParamType::Float, "u_BladeMinDensity",
						grass.m_MinDensityScale);
					per_draw.Overrides.emplace_back(ParamType::Float, "u_WindCullDistance",
						grass.m_WindCullDistance);

					prepare_draw(grass, per_draw);

					/* 底面先画：远距离抽稀、块边缘这些叶片稀疏的地方颜色不断档，
					 * 也不会从叶缝里看到裸地。每块一张四边形，代价可以忽略 */
					const auto& chunks = grass.m_Field->GetChunks();
					const auto& pad_buffer = grass.m_Field->GetPadBuffer();
					if (grass.m_DrawFlatUnderlay && pad_buffer != nullptr && !chunks.empty())
					{
						per_draw.Overrides[flat_pad_index].Value = 1;
						Renderer::SubmitInstanced(material, GrassField::GetPadPrimitive(), pad_buffer,
							0, static_cast<uint32_t>(chunks.size()), &per_draw, 0,
							&Renderer::GetGrassInstanceLayout());
						per_draw.Overrides[flat_pad_index].Value = 0;
					}

					/* 间接路径：GPU 已经剔除并压实好，按段提交。相机槽只备一段 */
					constexpr uint32_t kCameraSlot = 0;
					if (grass.m_Field->CanDrawIndirect(&render_view, kCameraSlot))
					{
						const uint32_t segments = grass.m_Field->GetIndirectSegments(&render_view, kCameraSlot);
						const uint32_t capacity = grass.m_Field->GetIndirectSegmentCapacity(&render_view, kCameraSlot);
						const uint32_t args_stride = GrassField::kIndirectArgsStride
							* static_cast<uint32_t>(sizeof(uint32_t));
						const uint32_t instance_stride = static_cast<uint32_t>(sizeof(GrassInstanceData));
						for (uint32_t segment = 0; segment < segments; ++segment)
						{
							Renderer::SubmitIndirect(material, blade,
								grass.m_Field->GetIndirectDrawBuffer(&render_view, kCameraSlot),
								grass.m_Field->GetIndirectArgs(&render_view, kCameraSlot), &per_draw,
								&Renderer::GetGrassInstanceLayout(), segment * args_stride,
								segment * capacity * instance_stride);
							RenderStatistics::Instance().AddGrassDrawCall();
						}
						continue;
					}

					/* 批表：连续块按实例预算合成一批，一笔画一批。
					 * 批比块大，剔除会多放行几块，多出来的株由着色器的距离抽稀丢掉，画面不变 */
					const auto& batches = grass.m_Field->GetBatches();
					if (GrassField::IsGlobalBatchingEnabled() && !batches.empty())
					{
						for (const GrassField::Batch& batch : batches)
						{
							if (cull && frustum.IsOutside(batch.Center, batch.Extent))
								continue;

							const float batch_radius = glm::length(batch.Extent);
							const float batch_distance = glm::distance(view_position, batch.Center) - batch_radius;
							if (batch_distance >= grass.m_EndCullDistance)
								continue;

							Renderer::SubmitInstanced(material, blade, instance_buffer, batch.First, batch.Count,
								&per_draw, 0, &Renderer::GetGrassInstanceLayout());
							RenderStatistics::Instance().AddGrassDrawCall();
						}
						continue;
					}

					for (const GrassField::Chunk& chunk : chunks)
					{
						/* 按观察点裁剪掉的块（Count = 0）不生成实例，也别画 */
						if (chunk.Count == 0)
							continue;
						if (cull && frustum.IsOutside(chunk.Center, chunk.Extent))
							continue;

						/* 按块到相机的最近距离判剔除：用块中心的话，大块在相机身边也会
						 * 因为中心远而被整块砍掉，画面就是"身边的草没了"。
						 * 半径取包围盒对角线，与 GPU 剔除的块包围球同一口径 */
						const float chunk_radius = glm::length(chunk.Extent);
						const float distance = glm::distance(view_position, chunk.Center) - chunk_radius;
						if (distance >= grass.m_EndCullDistance)
							continue;

						/* 距离抽稀逐株做（GrassBlade.glsl 的 GrassBladeFaded），这里整块提交：
						 * 按块截断实例数会在块边界上留台阶 */
						Renderer::SubmitInstanced(material, blade, instance_buffer, chunk.First, chunk.Count,
							&per_draw, 0, &Renderer::GetGrassInstanceLayout());
						RenderStatistics::Instance().AddGrassDrawCall();
					}
				}
			}
		}

		struct ScenePassData
		{
			FrameGraphResourceHandleTyped<FrameGraphTexture> ShadowMapHandle; /* 阴影贴图 */
		};

		void SubmitSceneObjects(RenderView& render_view, Scene& scene, bool transparent, SubmitBatch batch,
			DebugViewMode debug_view)
		{
			SubmitParams params;
			params.Batch = batch;
			params.DebugView = debug_view;
			SubmitForwardObjects(render_view, scene, transparent, params);
		}

		void SubmitSceneObjects(RenderView& render_view, Scene& scene, bool transparent, const SubmitParams& params)
		{
			SubmitForwardObjects(render_view, scene, transparent, params);
		}

		void FillGrassLightUniform(RenderView& render_view)
		{
			/* 草只吃主方向光：光照 UBO 一次只装一个光源，逐光源重画会让顶点量翻好几倍。
			 * 主阴影光源优先，没有阴影就取第一盏。 */
			const auto& lights = render_view.GetValidLights();
			if (lights.empty())
			{
				Renderer::FillEmptyLightUniformBuffer();
				return;
			}

			const ShadowMapManager* shadow_maps = render_view.GetShadowMapManager().get();
			const size_t primary = (shadow_maps != nullptr)
				? shadow_maps->FindPrimaryShadowLightIndex(lights) : 0;
			Renderer::FillLightUniformBuffer(lights[std::min(primary, lights.size() - 1)], shadow_maps);
		}

		void SubmitGrassFields(RenderView& render_view, Scene& scene, DebugViewMode debug_view)
		{
			PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

			FillGrassLightUniform(render_view);

			const auto probe_manager = scene.GetReflectionProbeManager();
			SubmitGrassDraws(render_view, scene,
				[](const GrassComponent& grass) { return grass.GetMaterial(); },
				[probe_manager, &render_view, debug_view](const GrassComponent& grass, DrawParams& per_draw)
				{
					if (debug_view != DebugViewMode::None && grass.GetMaterial()->SupportsDebugView())
						per_draw.Overrides.emplace_back(ParamType::Int, "u_DebugView", static_cast<int>(debug_view));

					if (!grass.GetMaterial()->SupportsIBL())
						return;

					const auto& field_chunks = grass.m_Field->GetChunks();
					const glm::vec3 center = field_chunks.empty() ? glm::vec3(0.0f) : field_chunks.front().Center;
					const auto chosen = (probe_manager != nullptr && probe_manager->HasProbe())
						? probe_manager->GetClostedReflectionProbe(center, render_view.GetCullingMask())
						: nullptr;
					Material::MakeIBLParamOverrides(per_draw.Overrides,
						chosen != nullptr ? chosen->GetIrradianceMap() : nullptr,
						chosen != nullptr ? chosen->GetPrefilterMap() : nullptr,
						probe_manager != nullptr ? probe_manager->GetBRDFLutMap() : nullptr);
				});
		}

		void SubmitGrassDebug(RenderView& render_view, Scene& scene, const SharedPtr<Material>& material)
		{
			PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

			SubmitGrassDraws(render_view, scene,
				[&material](const GrassComponent&) { return material; },
				[](const GrassComponent& grass, DrawParams& per_draw)
				{
					/* 外观参数跟着源材质走：一件调试材质服务整场草。
					 * 风场 / 脚印已经按 per-draw 写好了，材质里同名的值不能盖掉它 */
					for (const auto& [key, info] : grass.GetMaterial()->GetAllParameters())
					{
						(void)key;
						if (info.Type == ParamType::Texture)
							continue;
						const bool overridden = std::any_of(per_draw.Overrides.begin(), per_draw.Overrides.end(),
							[&info](const MaterialParamInfo& existing) { return existing.Name == info.Name; });
						if (!overridden)
							per_draw.Overrides.push_back(info);
					}
				});
		}

		bool HasVisibleGrassFields(const Scene& scene, LayerMask culling_mask)
		{
			for (auto [entity, grass] : scene.GetRegistry().view<GrassComponent>().each())
			{
				if (grass.GetMaterial() == nullptr || grass.m_Field == nullptr || grass.m_Field->IsEmpty())
					continue;
				if (scene.IsEntityInRenderView(entity, culling_mask))
					return true;
			}
			return false;
		}

		namespace
		{
			/* 运动矢量材质的底色：深度只测不写，深度是场景写好的那张 */
			SharedPtr<Material> MakeVelocityMaterial(const SharedPtr<DeviceShader>& shader)
			{
				auto material = CreateSharedPtr<Material>();
				material->SetShader(shader);
				auto& state = material->GetRasterState();
				state.EnableDepthTest = true;
				state.EnableDepthWrite = false;
				state.DepthCompareFunc = CompareFunc::GreaterEqual;
				return material;
			}

			/* 剔除方式跟源材质一致（草是双面），深度写一律关掉 */
			RenderRasterState MakeVelocityRasterState(const RenderRasterState& source)
			{
				RenderRasterState state = source;
				state.EnableDepthTest = true;
				state.EnableDepthWrite = false;
				state.DepthCompareFunc = CompareFunc::GreaterEqual;
				return state;
			}
		}

		void AddVelocityPass(RenderView* render_view, SubmitBatch batch,
			FrameGraphResourceHandleTyped<FrameGraphTexture> target)
		{
			PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

			if (render_view == nullptr) return;
			auto& frame_graph = render_view->GetFrameGraph();
			auto scene = render_view->GetOwnerScene();
			if (scene == nullptr) return;

			const auto& viewport_region = render_view->GetViewportRegion();
			if (viewport_region.Width == 0 || viewport_region.Height == 0) return;

			/* 深度按管线取：前向是场景 Pass 写的那张，延迟是 G-Buffer 的 */
			auto depth = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("ScenePassDepth");
			if (!depth)
				depth = frame_graph->GetBlackboard().GetResourceHandle<FrameGraphTexture>("GBufferDepth");
			if (!depth) return;

			/* 没有现成速度图就自己建一张（前向管线）；有的话只往上叠（延迟管线补草） */
			const bool owns_target = !target;

			static BuiltinAsset<DeviceShader> s_caster_shader("Shaders/Builtin/VelocityCaster.glsl");
			static BuiltinAsset<DeviceShader> s_grass_shader("Shaders/Grass/GrassVelocity.glsl");
			static BuiltinAsset<DeviceShader> s_clear_shader("Shaders/Builtin/VelocityClear.glsl");

			/* 材质一次建好反复用：逐绘制的差异全走 per-draw 覆盖，材质本身保持只读 */
			static SharedPtr<Material> s_caster_material;
			static SharedPtr<Material> s_grass_material;
			static SharedPtr<Material> s_clear_material;
			if (s_caster_material == nullptr || s_caster_material->GetShader() != s_caster_shader.Get())
				s_caster_material = MakeVelocityMaterial(s_caster_shader.Get());
			if (s_grass_material == nullptr || s_grass_material->GetShader() != s_grass_shader.Get())
				s_grass_material = MakeVelocityMaterial(s_grass_shader.Get());
			if (s_clear_material == nullptr || s_clear_material->GetShader() != s_clear_shader.Get())
				s_clear_material = Renderer::CreateFullScreenMaterial(s_clear_shader.Get());

			struct VelocityPassData
			{
				FrameGraphResourceHandleTyped<FrameGraphTexture> Depth;
				FrameGraphResourceHandleTyped<FrameGraphTexture> Velocity;
			};

			auto velocity_pass = frame_graph->AddPass<VelocityPassData>("VelocityPass",
				[&](FrameGraphBuilder& builder, VelocityPassData& data)
				{
					FrameGraphTexture::Descriptor velocity_desc;
					velocity_desc.Width = viewport_region.Width;
					velocity_desc.Height = viewport_region.Height;
					velocity_desc.TextureFormat = TextureFormat::RG16F;

					data.Depth = depth;
					data.Velocity = owns_target ? builder.CreateTexture("Velocity", velocity_desc) : target;
					builder.BindInputResource(data.Depth, FrameGraphTexture::Usage::DepthAttachment);
					if (owns_target)
					{
						builder.BindOutputResource(data.Velocity,
							FrameGraphTexture::Usage::ColorAttachment | FrameGraphTexture::Usage::Sampleable);
					}
					else
					{
						/* 叠到 G-Buffer 已经写好的那张上：产物是"写进已有资源"（同 Bloom 升采样） */
						builder.BindInputResource(data.Velocity, FrameGraphTexture::Usage::ColorAttachment);
					}

					FrameGraphPassInfo::Descriptor pass_desc;
					pass_desc.Attachments.ColorAttachments(0) = data.Velocity;
					pass_desc.Attachments.DepthAttachment() = data.Depth;
					pass_desc.ViewportRegion = viewport_region;
					/* 深度是场景写好的，必须保留；颜色的初值靠一笔全屏 0 铺出来 */
					pass_desc.PreserveContent = true;
					builder.CreateRenderPass("VelocityPassRenderTarget", pass_desc);
					builder.AsSideEffect();
				},
				[render_view, scene, batch, owns_target, caster = s_caster_material,
					grass_material = s_grass_material, clear = s_clear_material]
				(const FrameGraphResources& resources, const VelocityPassData& data)
				{
					const auto render_pass_info = resources.GetPassRenderTarget();
					render_view->EmplacePassFrameBuffer("VelocityPass", render_pass_info);

					render_pass_info->Bind();
					{
						/* 自己建的图先铺一遍 0：没被几何盖到的像素（天空 / 透明）当成不动。
						 * 叠到别人图上的时候不能铺，会把 G-Buffer 写好的速度抹掉 */
						if (owns_target)
							Renderer::Submit(clear, Renderer::GetFullScreenVertexArray());

						const auto& visible_objects = render_view->GetVisibleMeshObjects();
						const glm::vec3 view_position = (render_view->GetCullingCamera() != nullptr)
							? render_view->GetCullingCamera()->GetPosition() : glm::vec3(0.0f);
						const auto draw_list = (batch == SubmitBatch::All)
							? RenderSorting::CollectDrawList(visible_objects, false, view_position)
							: RenderSorting::CollectDrawList(visible_objects, false, view_position,
								[](const VisibleMeshObject& object)
								{
									return RenderSorting::IsForwardOnlyOpaque(object);
								});

						DrawParams per_draw;
						per_draw.FillMissing = MaterialFill::Values;
						for (const VisibleMeshObject* mesh_object : draw_list)
						{
							if (mesh_object->MeshSegment == nullptr || mesh_object->Material == nullptr)
								continue;

							Renderer::FillObjectUniformBuffer(*mesh_object);
							per_draw.RasterStateOverride = MakeVelocityRasterState(mesh_object->Material->GetRasterState());
							Renderer::Submit(caster, mesh_object->MeshSegment->GetMeshPrimitive(), &per_draw);
						}

						/* 草：叶形在顶点里展开，得用上一帧的风相位才能算出上一帧在哪 */
						SubmitGrassDraws(*render_view, *scene,
							[grass_material](const GrassComponent&) { return grass_material; },
							[](const GrassComponent& grass, DrawParams& per_draw_grass)
							{
								per_draw_grass.Overrides.emplace_back(ParamType::Float,
									"u_PrevWindPhase", grass.m_PrevWindPhase);
								per_draw_grass.RasterStateOverride =
									MakeVelocityRasterState(grass.GetMaterial()->GetRasterState());
							});

						/* 地形同样不在可见对象表里：静止几何的位移全部来自相机 */
						TerrainDraw::SubmitVelocity(*render_view, *scene);
					}
					render_pass_info->Unbind();
					Renderer::GetRenderAPI()->Flush();
				});

			/* 自己建的那张写进黑板：时域抗锯齿从这里取速度图 */
			if (owns_target)
				frame_graph->GetBlackboard()["Velocity"] = velocity_pass->GetData().Velocity;
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
			/* 场景 Pass 的提交体：不透明先画（写深度），透明最后（从远到近、只测不写）。
			 * 调试档位下材质着色器直接出读数，透明批不画（与延迟 G-Buffer 不收透明同口径） */
			void SubmitScenePassContent(RenderView& render_view, Scene& scene)
			{
				const DebugViewMode view_mode = render_view.GetDebugViewMode();
				const DebugViewMode debug_view = DebugView::GetForwardMaterialMode(view_mode);
				SubmitParams params;
				params.DebugView = debug_view;
				SubmitForwardObjects(render_view, scene, false, params);
				/* 地形不在可见对象表里，不透明批画完之后补一趟；草之后、透明之前 */
				TerrainDraw::SubmitForward(render_view, scene);
				SubmitGrassFields(render_view, scene, debug_view);
				if (debug_view == DebugViewMode::None && view_mode != DebugViewMode::Shadow)
					SubmitForwardObjects(render_view, scene, true, SubmitParams{});
			}

			/* 场景 → 视图纹理的公共体（颜色 + ObjectId + 深度三附件；颜色 / 深度落 Blackboard）：
			 * 编辑器视口的前向纹理路径与"默认目标 + 后处理"的旁路共用；调用方负责空指针守卫。 */
			FrameGraphResourceHandleTyped<FrameGraphTexture> AddScenePassToTextureCore(RenderView* render_view)
			{
				auto& frame_graph = render_view->GetFrameGraph();
				auto scene = render_view->GetOwnerScene();

				const auto& viewport_region = render_view->GetViewportRegion();

				/* 时域抗锯齿要读深度、要在 8bit 以上的缓冲里累积：这时场景色改 16F、
				 * 深度补 Sampleable、几何通道降到单采样（多采样深度采不了）。
				 * 关掉 TAA 时这三项原样不动，像素基线不受影响。 */
				const bool temporal = PostProcess::UsesTemporalJitter(*render_view);

				FrameGraphTexture::Descriptor color_desc;
				color_desc.Width = viewport_region.Width;
				color_desc.Height = viewport_region.Height;
				color_desc.TextureFormat = temporal ? TextureFormat::RGBA16F : TextureFormat::RGBA8;

				FrameGraphTexture::Descriptor object_id_desc;
				object_id_desc.Width = viewport_region.Width;
				object_id_desc.Height = viewport_region.Height;
				/* 实体 id 用浮点存：多重采样只对浮点 / 颜色格式做解析，
				 * 整数附件在开启 MSAA 时解析结果不可靠 */
				object_id_desc.TextureFormat = TextureFormat::R32F;

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
						/* 深度恒带 Sampleable：后处理里的雾要按场景深度算距离，
						 * 不依赖 TAA 是否开启 */
						builder.BindOutputResource(data.Depth,
							FrameGraphTexture::Usage::DepthAttachment | FrameGraphTexture::Usage::Sampleable);

						FrameGraphPassInfo::Descriptor pass_desc;
						pass_desc.Attachments.ColorAttachments(0) = data.Output;
						pass_desc.Attachments.ColorAttachments(1) = data.ObjectId;
						pass_desc.Attachments.DepthAttachment() = data.Depth;
						/* ObjectId 清成 -1（空像素不可拾取；与延迟 GBuffer 同款约定） */
						pass_desc.ColorClearValues.resize(2);
						pass_desc.ColorClearValues[1] = glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f);
						pass_desc.ViewportRegion = viewport_region;
						/* 场景几何开多重采样：物体边缘的锯齿靠它抹平。输出纹理本身仍是
						 * 单采样，通道结束由后端解析过去，叠加层与拾取照常读取。
						 * 开了时域抗锯齿就关掉：多采样深度不可采样，抖动也轮不到 MSAA 出力 */
						pass_desc.Multisample = !temporal;
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

							SubmitScenePassContent(*render_view, *scene);
						}
						render_pass_info->Unbind();
						Renderer::GetRenderAPI()->Flush();
					}
					);

				frame_graph->GetBlackboard()["ScenePassOutput"] = scene_pass->GetData().Output;
				frame_graph->GetBlackboard()["ScenePassDepth"] = scene_pass->GetData().Depth;
				/* 实体 id 附件留给叠加层（选中描边）与拾取共用 */
				frame_graph->GetBlackboard()["ScenePassObjectId"] = scene_pass->GetData().ObjectId;

				/* 运动矢量：时域抗锯齿按像素重投影，没有它动态物体就拖一条尾巴 */
				if (temporal)
					AddVelocityPass(render_view);

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

						SubmitScenePassContent(*render_view, *scene);
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
