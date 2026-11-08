#include "Pch.h"
#include "TerrainPasses.h"

#include <Helios/Core/AssetManager.h>
#include <Helios/Renderer/Frustum.h>
#include <Helios/Renderer/FrameGraph/FrameGraph.h>
#include <Helios/Renderer/Renderer.h>
#include <Helios/Renderer/RenderPasses/DebugViewPasses.h>
#include <Helios/Renderer/RenderView.h>
#include <Helios/Scene/Camera.h>
#include <Helios/Scene/Components.h>
#include <Helios/Scene/Material.h>
#include <Helios/Scene/Scene.h>
#include <Helios/Scene/SceneCommon.h>
#include <Helios/Scene/ShadowMap.h>
#include <Helios/Scene/TerrainField.h>
#include <Helios/Scene/TerrainVirtualTexture.h>
#include <Helios/VirtualDevice/DeviceShader.h>

namespace Helios::TerrainDraw
{
	namespace
	{
		/* 一块地形在本次提交里占的实例区间 */
		struct TerrainBatch
		{
			const TerrainComponent* Component{ nullptr };
			const TerrainField* Field{ nullptr };
			int EntityId{ -1 };
			uint32_t First{ 0 };
			uint32_t Count{ 0 };
			/* patch 由 GPU 选出来：实例缓冲与间接参数按视图各自一份，
			 * First / Count 不参与提交（数量在参数块里） */
			bool Gpu{ false };
			SharedPtr<DeviceVertexBuffer> GpuInstances;
			SharedPtr<DeviceStorageBuffer> GpuArgs;
		};

		/* 一帧里 G-Buffer / 速度 / 前向要的是同一份 patch 列表（都按相机视锥选），
		 * 按视图缓存一行，省掉同帧的重复选择与重复上传。阴影按各自的级联盒子选，不在这份里 */
		struct FrameCache
		{
			const RenderView* View{ nullptr };
			uint32_t Frame{ std::numeric_limits<uint32_t>::max() };
			std::vector<TerrainPatch> Patches;
			std::vector<TerrainBatch> Batches;
			Renderer::InstanceRange Range;
		};

		FrameCache& AcquireCache(const RenderView* view)
		{
			static FrameCache s_Caches[2];

			for (FrameCache& cache : s_Caches)
			{
				if (cache.View == view)
					return cache;
			}

			/* 没见过这个视图：先用空行，再顶掉帧号小的那一行 */
			FrameCache& slot = (s_Caches[0].View == nullptr
				|| (s_Caches[1].View != nullptr && s_Caches[0].Frame <= s_Caches[1].Frame))
				? s_Caches[0] : s_Caches[1];
			slot.View = view;
			slot.Frame = std::numeric_limits<uint32_t>::max();
			return slot;
		}

		/* 地形只吃主方向光：先把光照 UBO 填成主阴影光源，与草地同一份口径 */
		void FillPrimaryLight(RenderView& view)
		{
			const auto& lights = view.GetValidLights();
			if (lights.empty())
			{
				Renderer::FillEmptyLightUniformBuffer();
				return;
			}

			const ShadowMapManager* shadow_maps = view.GetShadowMapManager().get();
			const size_t primary = (shadow_maps != nullptr)
				? shadow_maps->FindPrimaryShadowLightIndex(lights) : 0;
			Renderer::FillLightUniformBuffer(lights[std::min(primary, lights.size() - 1)], shadow_maps);
		}

		/* 前向材质：从组件的材质克隆一份换到前向变体。参数名与延迟同源，
		 * 换完直接沿用；源材质参数版本变了才重建。源材质释放后地址可能被新材质复用，
		 * 所以记一份弱引用核对身份，顺手清掉已经释放的 */
		SharedPtr<Material> GetForwardMaterial(const SharedPtr<Material>& source)
		{
			struct Entry
			{
				std::weak_ptr<Material> Source;
				SharedPtr<Material> Material;
				uint32_t Revision{ 0 };
			};
			static std::unordered_map<const Material*, Entry> s_Cache;

			for (auto it = s_Cache.begin(); it != s_Cache.end();)
				it = it->second.Source.expired() ? s_Cache.erase(it) : std::next(it);

			Entry& entry = s_Cache[source.get()];
			if (entry.Material != nullptr && entry.Revision == source->GetRevision()
				&& entry.Source.lock() == source)
				return entry.Material;

			/* 同一份地形着色器的前向变体，见源码里的 @variant */
			static BuiltinAsset<DeviceShader> s_forward_shader("Shaders/Terrain/Terrain.glsl",
				ShaderVariantConfig{ ShaderVariantDefines{ { "TERRAIN_FORWARD", 1 } } });
			const auto shader = s_forward_shader.Get();
			if (shader == nullptr)
				return source;

			auto material = source->Clone();
			material->SetShader(shader);
			entry.Source = source;
			entry.Material = material;
			entry.Revision = source->GetRevision();
			return material;
		}

		/* 高度图与覆盖范围：材质本身多块地形共用，逐地形的差异全走 per-draw 覆盖。
		 * 调用方给的 per_draw 要是空的；要跟材质的参数叠加时先抄材质再调这里，地形数据后写覆盖 */
		void AddTerrainOverrides(const TerrainField& field, DrawParams& per_draw)
		{
			per_draw.FillMissing = MaterialFill::Values | MaterialFill::Textures;
			per_draw.Overrides.push_back(Material::MakeTextureParam("u_HeightMap", field.GetHeightMap()));
			/* 几何在局部空间，世界位置与法线靠这三个矩阵 */
			per_draw.Overrides.emplace_back(ParamType::Mat4, "u_TerrainModel", field.GetWorldTransform());
			per_draw.Overrides.emplace_back(ParamType::Mat4, "u_TerrainInverse", field.GetWorldInverse());
			per_draw.Overrides.emplace_back(ParamType::Mat4, "u_TerrainNormalMatrix", field.GetNormalMatrix());
			per_draw.Overrides.emplace_back(ParamType::Vec2, "u_TerrainExtent", field.GetExtent());
			per_draw.Overrides.emplace_back(ParamType::Vec2, "u_TerrainWorldExtent",
				field.GetWorldHalfExtent());
			per_draw.Overrides.emplace_back(ParamType::Vec3, "u_TerrainOrigin", field.GetOrigin());
			per_draw.Overrides.emplace_back(ParamType::Vec2, "u_HeightmapSize",
				glm::vec2(static_cast<float>(field.GetResolution())));
			per_draw.Overrides.emplace_back(ParamType::Float, "u_HeightScale", field.GetHeightScale());

			/* 法线差分步长按高度图纹素推（两个纹素），所有 LOD 共用：
			 * 每块 patch 各用各的话，坡度的混色会沿 LOD 交界切出一条直带 */
			const float texel = (field.GetExtent().x * 2.0f)
				/ static_cast<float>(std::max(field.GetResolution(), 2u) - 1u);
			per_draw.Overrides.emplace_back(ParamType::Float, "u_TerrainNormalStep",
				std::max(texel * 2.0f, 0.05f));

			/* morph 开关（检查程序对照用）：默认 1，不改就沿用着色器里的值 */
			const float morph_scale = TerrainField::GetGlobalMorphScale();
			if (morph_scale != 1.0f)
				per_draw.Overrides.emplace_back(ParamType::Float, "u_TerrainMorphScale", morph_scale);
		}

		/* 高度图与地形参数：所有地形着色器（可见 / 阴影 / 速度 / 页烘焙）都声明了这些 */
		void AddVirtualTextureParams(const TerrainField& field, DrawParams& per_draw)
		{
			const TerrainVirtualTexture& virtual_texture = field.GetVirtualTexture();

			/* 页池是 2D 数组，没启用时也得绑数组（1×1×1 占位），绑 2D 白图会触发 Metal 校验断言 */
			const SharedPtr<DeviceTexture>& fallback_atlas = TerrainVirtualTexture::GetFallbackAtlas();
			const SharedPtr<DeviceTexture>& page_table = virtual_texture.GetPageTable();
			const SharedPtr<DeviceTexture>& page_pool = virtual_texture.GetPool();
			const bool vt_ready = page_table != nullptr && page_pool != nullptr
				&& virtual_texture.GetGrid().x > 0.0f;

			per_draw.Overrides.push_back(Material::MakeTextureParam("u_VTPageTable",
				vt_ready ? page_table : DeviceTexture::White()));
			per_draw.Overrides.push_back(Material::MakeTextureParam("u_VTPageAtlas",
				vt_ready ? page_pool : (fallback_atlas != nullptr ? fallback_atlas : DeviceTexture::White())));
			per_draw.Overrides.emplace_back(ParamType::Vec2, "u_VTGrid",
				vt_ready ? virtual_texture.GetGrid() : glm::vec2(0.0f));

			/* 远景底图：没烘出来时矩形全 0，着色器直接跳过，采样器仍要有绑定。
			 * 只在虚拟纹理开着时启用：关掉 VT 的基准就该是逐像素直接算 */
			const SharedPtr<DeviceTexture>& basemap = field.GetBasemap();
			per_draw.Overrides.push_back(Material::MakeTextureParam("u_Basemap",
				basemap != nullptr ? basemap : DeviceTexture::White()));
			per_draw.Overrides.emplace_back(ParamType::Vec4, "u_BasemapRect",
				TerrainVirtualTexture::IsEnabled() ? field.GetBasemapRect() : glm::vec4(0.0f));
		}

		/* 可见着色的逐绘制参数：是否接收阴影 + 虚拟纹理 + 拾取 id。只有声明了这些参数的
		 * 着色器才能用，名字反射不到时 ApplyParam 会回退到槽位 0，把高度图挤掉 */
		void BuildDrawParams(const TerrainField& field, const TerrainComponent& component,
			int entity_id, DrawParams& per_draw)
		{
			AddTerrainOverrides(field, per_draw);
			per_draw.Overrides.emplace_back(ParamType::Float, "u_TerrainReceiveShadow",
				component.m_ReceiveShadow ? 1.0f : 0.0f);
			per_draw.Overrides.emplace_back(ParamType::Int, "u_TerrainObjectId", entity_id);
			AddVirtualTextureParams(field, per_draw);
		}

		/* 按给定视锥选 patch 并上传，返回本帧实例环里的位置。
		 * batches 记录每块地形占的区间，调用方据此逐块提交。
		 * allow_gpu 为真时，已在视图开始前由 GPU 选过 patch 的地形不上实例环，
		 * 提交时走它自己的间接绘参数 */
		bool BuildAndUpload(Scene& scene, LayerMask culling_mask, const Frustum& frustum, const glm::vec3& eye,
			bool allow_gpu, const RenderView* view, std::vector<TerrainPatch>& patches,
			std::vector<TerrainBatch>& batches, Renderer::InstanceRange& range)
		{
			patches.clear();
			batches.clear();

			const uint32_t frame_index = scene.GetFrameIndex();
			for (auto [entity, component] : scene.GetRegistry().view<TerrainComponent>().each())
			{
				if (component.GetMaterial() == nullptr)
					continue;
				if (!scene.IsEntityInView(entity, culling_mask))
					continue;

				if (component.m_Field == nullptr)
					component.m_Field = CreateSharedPtr<TerrainField>();

				/* 参数或变换变了才重建，内部有键短路 */
				component.m_Field->Rebuild(component, scene.GetRenderWorldTransform(entity));
				if (component.m_Field->IsEmpty())
					continue;

				/* GPU 这条路自己选自己画，patch 不回 CPU */
				if (allow_gpu && view != nullptr && component.m_Field->HasSelection(frame_index, view))
				{
					TerrainBatch batch;
					batch.Component = &component;
					batch.Field = component.m_Field.get();
					batch.EntityId = static_cast<int>(entity);
					batch.Gpu = true;
					batch.GpuInstances = component.m_Field->GetSelectedInstances(view);
					batch.GpuArgs = component.m_Field->GetSelectedArgs(view);
					batches.push_back(std::move(batch));
					continue;
				}

				const uint32_t first = static_cast<uint32_t>(patches.size());
				component.m_Field->BuildPatches(frustum, eye, patches);
				if (patches.size() == first)
					continue;

				batches.push_back(TerrainBatch{ &component, component.m_Field.get(),
					static_cast<int>(entity), first, static_cast<uint32_t>(patches.size()) - first, false });
			}

			if (batches.empty())
				return false;

			/* 全走 GPU 时没有 CPU 侧 patch，实例环不用碰 */
			if (patches.empty())
				return true;

			range = Renderer::UploadInstances(patches.data(), static_cast<uint32_t>(patches.size()),
				static_cast<uint32_t>(sizeof(TerrainPatch)));
			return range.Buffer != nullptr;
		}

		const FrameCache& EnsureVisibleFrame(RenderView& view, Scene& scene)
		{
			FrameCache& cache = AcquireCache(&view);
			const uint32_t frame = scene.GetFrameIndex();
			if (cache.Frame == frame)
				return cache;

			cache.Frame = frame;
			cache.Range = {};

			const Camera* camera = view.GetCullingCamera();
			if (camera == nullptr)
			{
				cache.Patches.clear();
				cache.Batches.clear();
				return cache;
			}

			const Frustum frustum = Frustum::FromViewProjection(camera->GetViewProjectionMatrix());
			BuildAndUpload(scene, view.GetCullingMask(), frustum, camera->GetPosition(),
				TerrainField::IsGpuSelectionEnabled(), &view, cache.Patches, cache.Batches, cache.Range);
			return cache;
		}

		/* 一笔地形：GPU 选的走间接绘制，CPU 选的走实例环 */
		void SubmitBatch(const SharedPtr<Material>& material, const FrameCache& frame,
			const TerrainBatch& batch, const DrawParams& per_draw)
		{
			if (batch.Gpu)
			{
				Renderer::SubmitIndirect(material, TerrainField::GetPatchPrimitive(),
					batch.GpuInstances, batch.GpuArgs, &per_draw,
					&TerrainField::GetInstanceLayout());
				return;
			}

			if (frame.Range.Buffer == nullptr)
				return;

			Renderer::SubmitInstanced(material, TerrainField::GetPatchPrimitive(), frame.Range.Buffer,
				frame.Range.FirstInstance + batch.First, batch.Count, &per_draw, 0,
				&TerrainField::GetInstanceLayout());
		}

		void SubmitVisibleBatch(const FrameCache& frame, bool forward, DebugViewMode debug_view)
		{
			if (frame.Batches.empty())
				return;

			for (const TerrainBatch& batch : frame.Batches)
			{
				const SharedPtr<Material> source = batch.Component->GetMaterial();
				if (source == nullptr)
					continue;

				const SharedPtr<Material> final_material = forward ? GetForwardMaterial(source) : source;
				if (final_material == nullptr)
					continue;

				DrawParams per_draw;
				BuildDrawParams(*batch.Field, *batch.Component, batch.EntityId, per_draw);

				/* 前向的读数由材质自己出；延迟的 Surface / Shadow 档读 G-Buffer，不用管 */
				if (forward && debug_view != DebugViewMode::None)
				{
					per_draw.VariantDefines["DEBUG_VIEW"] = 1;
					per_draw.Overrides.emplace_back(ParamType::Int, "u_DebugView",
						static_cast<int>(debug_view));
				}

				SubmitBatch(final_material, frame, batch, per_draw);
			}
		}

		/* 投影材质：按级联下标取光照 UBO 里的矩阵，一份材质服务一层的所有地形。
		 * 着色器换了（热重载）就整批重建 */
		SharedPtr<Material> GetShadowMaterial(uint8_t cascade_id)
		{
			static constexpr uint32_t kSlots = 8;
			static SharedPtr<Material> s_Materials[kSlots];
			static SharedPtr<DeviceShader> s_Shader;

			const auto shader = AssetManager::Instance().GetOrLoad<DeviceShader>(
				ABSOLUTE_PATH("Shaders/Terrain/TerrainShadow.glsl"));
			if (shader == nullptr)
				return nullptr;

			if (s_Shader != shader)
			{
				for (auto& material : s_Materials)
					material = nullptr;
				s_Shader = shader;
			}

			SharedPtr<Material>& material = s_Materials[cascade_id % kSlots];
			if (material == nullptr)
			{
				material = Material::Create(shader);
				material->SetParameters(ParamType::Int, "u_CascadeIndex", static_cast<int>(cascade_id));
			}
			return material;
		}

		/* 源材质的参数整份抄进 per-draw：页烘焙要跟可见着色用同一套外观（含层贴图）。
		 * 只抄烘焙着色器声明过的 —— 反射不到的参数会被兜到槽位 0，把高度图挤掉 */
		void CopyMaterialParams(const Material& source, const DeviceShader& shader, DrawParams& per_draw)
		{
			for (const auto& [key, info] : source.GetAllParameters())
			{
				(void)key;
				if (info.Type == ParamType::Texture && shader.GetUniformBinding(info.Name) < 0)
					continue;
				per_draw.Overrides.push_back(info);
			}
		}

		/* 页烘焙材质：整页一笔全屏四边形，逐页的差异全走 per-draw */
		SharedPtr<Material> GetVirtualTextureBakeMaterial()
		{
			static SharedPtr<Material> s_Material;
			static SharedPtr<DeviceShader> s_Shader;

			const auto shader = AssetManager::Instance().GetOrLoad<DeviceShader>(
				ABSOLUTE_PATH("Shaders/Terrain/TerrainVTPage.glsl"));
			if (shader == nullptr)
				return nullptr;

			if (s_Shader != shader)
			{
				s_Material = nullptr;
				s_Shader = shader;
			}
			if (s_Material == nullptr)
			{
				s_Material = Material::Create(shader);
				auto& state = s_Material->GetRasterState();
				state.EnableDepthTest = false;
				state.EnableDepthWrite = false;
			}
			return s_Material;
		}

		/* 运动矢量材质：深度只测不写，深度是场景已经写好的那张 */
		SharedPtr<Material> GetVelocityMaterial()
		{
			static SharedPtr<Material> s_Material;
			static SharedPtr<DeviceShader> s_Shader;

			const auto shader = AssetManager::Instance().GetOrLoad<DeviceShader>(
				ABSOLUTE_PATH("Shaders/Terrain/TerrainVelocity.glsl"));
			if (shader == nullptr)
				return nullptr;

			if (s_Shader != shader)
			{
				s_Material = nullptr;
				s_Shader = shader;
			}

			if (s_Material == nullptr)
			{
				s_Material = Material::Create(shader);
				auto& state = s_Material->GetRasterState();
				state.EnableDepthTest = true;
				state.EnableDepthWrite = false;
				state.DepthCompareFunc = CompareFunc::GreaterEqual;
			}
			return s_Material;
		}
	}

	bool HasVisibleTerrain(const Scene& scene, LayerMask culling_mask)
	{
		for (auto [entity, component] : scene.GetRegistry().view<TerrainComponent>().each())
		{
			if (component.GetMaterial() == nullptr)
				continue;
			if (scene.IsEntityInView(entity, culling_mask))
				return true;
		}
		return false;
	}

	/* 节点表是输入，压实实例与间接参数是输出，参数块走 4 号槽 */
	bool DispatchSelect(const SharedPtr<DeviceStorageBuffer>& nodes,
		const SharedPtr<DeviceStorageBuffer>& patches,
		const SharedPtr<DeviceStorageBuffer>& args,
		const void* param_data, uint32_t param_size, uint32_t node_count)
	{
		const SharedPtr<RenderAPI>& api = Renderer::GetRenderAPI();
		if (api == nullptr || nodes == nullptr || patches == nullptr || args == nullptr
			|| node_count == 0 || !api->SupportsCompute())
			return false;

		static BuiltinAsset<DeviceShader> s_select_shader("Shaders/Terrain/TerrainSelect.glsl");
		const SharedPtr<DeviceShader> shader = s_select_shader.Get();
		if (shader == nullptr || !shader->IsCompute())
			return false;

		const std::vector<std::pair<uint32_t, SharedPtr<DeviceStorageBuffer>>> buffers = {
			{ 1, nodes }, { 2, patches }, { 3, args },
		};
		const uint32_t group_size = std::max(1u, shader->GetThreadGroupSize());
		const uint32_t group_count = (node_count + group_size - 1) / group_size;
		return api->DispatchCompute(shader, buffers, 4, param_data, param_size, group_count);
	}

	void AddSelectPass(RenderView& view)
	{
		if (!TerrainField::IsGpuSelectionEnabled())
			return;

		const SharedPtr<Scene> scene = view.GetOwnerScene();
		if (scene == nullptr || view.GetCullingCamera() == nullptr)
			return;

		struct SelectPassData
		{
			TerrainField* Field{ nullptr };
			TerrainField::SelectionResources Resources;
		};

		for (auto [entity, terrain] : scene->GetRegistry().view<TerrainComponent>().each())
		{
			if (terrain.GetMaterial() == nullptr || !scene->IsEntityInView(entity, view.GetCullingMask()))
				continue;
			if (terrain.m_Field == nullptr)
				terrain.m_Field = CreateSharedPtr<TerrainField>();

			/* 高度图与节点表要和这一帧的变换对齐，后面提交只查帧号 */
			terrain.m_Field->Rebuild(terrain, scene->GetRenderWorldTransform(entity));
			if (terrain.m_Field->IsEmpty())
				continue;

			TerrainField::SelectionResources resources;
			if (!terrain.m_Field->AcquireSelectionResources(&view, resources))
				continue;

			const std::string pass_name = "TerrainSelectPass_" + std::to_string(static_cast<uint32_t>(entity));
			TerrainField* field = terrain.m_Field.get();

			view.GetFrameGraph()->AddPass<SelectPassData>(pass_name,
				[&view, field, resources, pass_name](FrameGraphBuilder& builder, SelectPassData& data)
				{
					data.Field = field;
					data.Resources = resources;

					/* 节点表是输入，压实实例与间接参数是输出。三块都是跨帧保留的，
					 * 用 Import 进来，图只记录谁写它们，帧末不回收 */
					const SharedPtr<FrameGraph> frame_graph = view.GetFrameGraph();
					const auto nodes = frame_graph->ImportBuffer(pass_name + "_Nodes", resources.Nodes);
					const auto patches = frame_graph->ImportBuffer(pass_name + "_Patches", resources.Patches);
					const auto args = frame_graph->ImportBuffer(pass_name + "_Args", resources.Args);

					builder.BindInputResource(nodes, BufferAccess::Read);
					builder.BindOutputResource(patches, BufferAccess::Write);
					builder.BindOutputResource(args, BufferAccess::Write);

					/* 绘制端还没把这几块缓冲绑成输入，先靠保活让 Pass 留下。
					 * 计算 Pass 不绑默认渲染目标 */
					builder.AsSideEffect();
					builder.AsComputePass();
				},
				[&view, scene](const FrameGraphResources&, const SelectPassData& data)
				{
					const Camera* camera = view.GetCullingCamera();
					if (camera == nullptr || data.Field == nullptr)
						return;

					const Frustum frustum = Frustum::FromViewProjection(camera->GetViewProjectionMatrix());
					data.Field->DispatchSelection(data.Resources, frustum.Planes, camera->GetPosition(),
						scene->GetFrameIndex(), &view);
				});
		}
	}

	void BakeVirtualTextures(RenderView& view, Scene& scene, const TerrainField* only_field,
		const SharedPtr<DeviceFrameBuffer>& page_frame)
	{
		if (!TerrainVirtualTexture::IsEnabled())
			return;

		const SharedPtr<Material> material = GetVirtualTextureBakeMaterial();
		if (material == nullptr)
			return;

		const FrameCache& frame = EnsureVisibleFrame(view, scene);
		/* patch 全在 GPU 上选的时候 CPU 列表是空的，别在这里挡掉 */
		if (frame.Batches.empty())
			return;

		for (const TerrainBatch& batch : frame.Batches)
		{
			if (only_field != nullptr && batch.Field != only_field)
				continue;
			if (batch.Component == nullptr || batch.Field == nullptr)
				continue;
			/* GPU 选的那批 patch 数量在参数块里，CPU 侧 Count 是 0 */
			if (!batch.Gpu && batch.Count == 0)
				continue;

			TerrainVirtualTexture& virtual_texture = batch.Component->m_Field->GetVirtualTexture();
			if (!virtual_texture.EnsureResources())
				continue;

			/* 远景底图：整块地形烘一张，页没驻留的远处像素采它。
			 * 与页共用同一个烘焙材质，只在矩形上换成整块地形 */
			const uint32_t material_revision = batch.Component->GetMaterial() != nullptr
				? batch.Component->GetMaterial()->GetRevision() : 0u;
			if (batch.Field->EnsureBasemap() && batch.Field->NeedsBasemap(material_revision))
			{
				const SharedPtr<DeviceFrameBuffer>& basemap_frame = batch.Field->GetBasemapFrameBuffer();
				if (basemap_frame != nullptr)
				{
					basemap_frame->Bind();

					DrawParams basemap_draw;
					if (batch.Component->GetMaterial() != nullptr)
						CopyMaterialParams(*batch.Component->GetMaterial(), *material->GetShader(), basemap_draw);
					AddTerrainOverrides(*batch.Field, basemap_draw);

					/* 底图矩形按页烘焙着色器的约定给：x / z 是左下角，w 是边长 */
					const glm::vec4 rect = batch.Field->GetBasemapRect();
					basemap_draw.Overrides.emplace_back(ParamType::Vec4, "u_VTPageRect",
						glm::vec4(rect.x, 0.0f, rect.y, rect.z));
					Renderer::Submit(material, Renderer::GetFullScreenVertexArray(), &basemap_draw);
					basemap_frame->Unbind();
					batch.Field->MarkBasemapBaked(material_revision);
				}
			}

			/* 缺页在建图阶段就挑好了，这里只负责烘 */
			if (!virtual_texture.HasPending())
				continue;

			/* 页池的一层当颜色附件：图里给了帧缓冲就用它的（页池已作为图资源），
			 * 否则退回 VT 自己建的那份 */
			const SharedPtr<DeviceFrameBuffer>& frame_buffer = page_frame != nullptr
				? page_frame : virtual_texture.GetBakeFrameBuffer();
			if (frame_buffer == nullptr)
				continue;

			const glm::vec3 origin = batch.Field->GetOrigin();
			/* 页按世界空间切（与 TerrainVirtualTexture::Prepare 同一份范围） */
			const glm::vec2 world_half = batch.Field->GetWorldHalfExtent();
			const glm::vec2 min_corner(origin.x - world_half.x, origin.z - world_half.y);

			for (const glm::uvec3& page : virtual_texture.GetPending())
			{
				/* 页池的一层当颜色附件：一次只写一层，烘完解绑，主通道照着自己绑 */
				frame_buffer->Bind(FrameBufferBindInfo::ToColorLayer(0, static_cast<uint16_t>(page.z), 0));

				DrawParams per_draw;
				/* 先把材质的外观参数抄过来（层颜色 / 层贴图），再写地形自己的数据：
				 * 后写的覆盖先写的，高度图与矩阵不会被材质里的旧值顶掉 */
				if (batch.Component->GetMaterial() != nullptr)
					CopyMaterialParams(*batch.Component->GetMaterial(), *material->GetShader(), per_draw);
				AddTerrainOverrides(*batch.Field, per_draw);

				const glm::vec2 page_min = min_corner
					+ glm::vec2(static_cast<float>(page.x), static_cast<float>(page.y))
					* TerrainVirtualTexture::kPageWorldSize;
				per_draw.Overrides.emplace_back(ParamType::Vec4, "u_VTPageRect",
					glm::vec4(page_min.x, 0.0f, page_min.y, TerrainVirtualTexture::kPageWorldSize));

				Renderer::Submit(material, Renderer::GetFullScreenVertexArray(), &per_draw);
				frame_buffer->Unbind();
			}

			virtual_texture.Commit();
		}
	}

	void SubmitGBuffer(RenderView& view, Scene& scene)
	{
		const FrameCache& frame = EnsureVisibleFrame(view, scene);
		SubmitVisibleBatch(frame, false, DebugViewMode::None);
	}

	void AddVirtualTextureBakePass(RenderView& view)
	{
		if (!TerrainVirtualTexture::IsEnabled())
			return;

		const SharedPtr<Scene> scene = view.GetOwnerScene();
		const Camera* camera = view.GetCullingCamera();
		if (scene == nullptr || camera == nullptr)
			return;

		struct BakePassData
		{
			TerrainField* Field{ nullptr };
		};

		const uint32_t page_texels = TerrainVirtualTexture::GetPageTexels();
		const uint32_t frame_index = scene->GetFrameIndex();
		const glm::vec3 eye = camera->GetPosition();
		const Frustum bake_frustum = Frustum::FromViewProjection(camera->GetViewProjectionMatrix());

		for (auto [entity, terrain] : scene->GetRegistry().view<TerrainComponent>().each())
		{
			if (terrain.GetMaterial() == nullptr || !scene->IsEntityInView(entity, view.GetCullingMask()))
				continue;
			if (terrain.m_Field == nullptr || terrain.m_Field->IsEmpty())
				continue;

			TerrainVirtualTexture& virtual_texture = terrain.m_Field->GetVirtualTexture();
			if (!virtual_texture.EnsureResources() || virtual_texture.GetPool() == nullptr)
				continue;

			/* 挑页放在建图阶段：PrepareFrustum 只看视锥，不依赖 patch 列表，
			 * 所以不用碰 EnsureVisibleFrame，也就不会把 GPU 选择的结果降级成 CPU 路径。
			 * 执行阶段只烘不挑 —— 同一帧挑两次会让层分配漂移 */
			virtual_texture.PrepareFrustum(*terrain.m_Field, bake_frustum.Planes, eye, frame_index);

			/* 缺页和远景底图都没有要补的，就不占一个 Pass */
			const bool need_pages = virtual_texture.HasPending();
			const bool need_basemap = terrain.m_Field->EnsureBasemap()
				&& terrain.m_Field->NeedsBasemap(terrain.GetMaterial()->GetRevision());
			if (!need_pages && !need_basemap)
				continue;

			const std::string pass_name = "TerrainVTBakePass_" + std::to_string(static_cast<uint32_t>(entity));
			TerrainField* field = terrain.m_Field.get();
			const auto pool = view.GetFrameGraph()->ImportTexture(pass_name + "_Pool", virtual_texture.GetPool());

			view.GetFrameGraph()->AddPass<BakePassData>(pass_name,
				[pool, pass_name, page_texels](FrameGraphBuilder& builder, BakePassData& data)
				{
					data.Field = nullptr;

					/* 页池作为颜色附件：一层一页，执行时按页号切层 */
					FrameGraphPassInfo::Descriptor desc;
					desc.Attachments.ColorAttachments(0) = pool;
					desc.ViewportRegion = ViewportRegion{ 0, 0, page_texels, page_texels };
					builder.CreateRenderPass(pass_name + "_Target", desc);
					builder.BindOutputResource(pool, TextureUsage::ColorAttachment);

					/* 页池跨帧保留、采样端在别的 Pass 里，先靠保活留下这一趟 */
					builder.AsSideEffect();
				},
				[&view, scene, field](const FrameGraphResources& resources, const BakePassData&)
				{
					/* 图里给的帧缓冲指向这块地形的页池，按页号切层在内部做 */
					BakeVirtualTextures(view, *scene, field, resources.GetPassRenderTarget());
				});
		}
	}

	void SubmitForward(RenderView& view, Scene& scene, DebugViewMode debug_view)
	{
		FillPrimaryLight(view);

		/* 没指定档位时按视图本帧的模式走：Surface 与光照分量都由材质自己出 */
		const DebugViewMode mode = (debug_view != DebugViewMode::None)
			? debug_view : DebugView::GetForwardMaterialMode(view.GetDebugViewMode());

		const FrameCache& frame = EnsureVisibleFrame(view, scene);
		SubmitVisibleBatch(frame, true, mode);
	}

	void SubmitDebug(RenderView& view, Scene& scene, const SharedPtr<Material>& material)
	{
		const FrameCache& frame = EnsureVisibleFrame(view, scene);
		if (frame.Batches.empty() || material == nullptr)
			return;

		for (const TerrainBatch& batch : frame.Batches)
		{
			const SharedPtr<Material> source = batch.Component->GetMaterial();
			if (source == nullptr)
				continue;

			/* 外观参数（含层贴图）先从源材质抄过来，读数才跟正常着色一致；
			 * 地形自己的高度图与范围后写，共用材质里的旧值顶不掉它 */
			DrawParams per_draw;
			CopyMaterialParams(*source, *material->GetShader(), per_draw);
			AddTerrainOverrides(*batch.Field, per_draw);
			AddVirtualTextureParams(*batch.Field, per_draw);

			SubmitBatch(material, frame, batch, per_draw);
		}
	}

	void SubmitVelocity(RenderView& view, Scene& scene)
	{
		const SharedPtr<Material> material = GetVelocityMaterial();
		if (material == nullptr)
			return;

		const FrameCache& frame = EnsureVisibleFrame(view, scene);
		if (frame.Batches.empty())
			return;

		for (const TerrainBatch& batch : frame.Batches)
		{
			const SharedPtr<Material> source = batch.Component->GetMaterial();
			if (source == nullptr)
				continue;

			DrawParams per_draw;
			AddTerrainOverrides(*batch.Field, per_draw);

			/* 剔除方式跟源材质一致，深度写一律关掉 */
			RenderRasterState state = source->GetRasterState();
			state.EnableDepthTest = true;
			state.EnableDepthWrite = false;
			state.DepthCompareFunc = CompareFunc::GreaterEqual;
			per_draw.RasterStateOverride = state;

			SubmitBatch(material, frame, batch, per_draw);
		}
	}

	void SubmitShadow(RenderView& view, Scene& scene, const glm::mat4& light_view_projection,
		uint8_t cascade_id)
	{
		const Camera* camera = view.GetCullingCamera();
		if (camera == nullptr)
			return;

		/* patch 按本级联的盒子选：视锥外的山体照样会把影子甩进画面，
		 * 只按相机视锥选会漏掉这一批。LOD 仍按相机距离，跟可见着色对得上 */
		const Frustum frustum = Frustum::FromViewProjection(light_view_projection);

		std::vector<TerrainPatch> patches;
		std::vector<TerrainBatch> batches;
		Renderer::InstanceRange range;
		if (!BuildAndUpload(scene, view.GetCullingMask(), frustum, camera->GetPosition(), false, nullptr,
			patches, batches, range))
			return;
		const SharedPtr<Material> material = GetShadowMaterial(cascade_id);
		if (material == nullptr)
			return;

		for (const TerrainBatch& batch : batches)
		{
			if (batch.Component == nullptr || !batch.Component->m_CastShadow)
				continue;

			DrawParams per_draw;
			AddTerrainOverrides(*batch.Field, per_draw);

			Renderer::SubmitInstanced(material, TerrainField::GetPatchPrimitive(), range.Buffer,
				range.FirstInstance + batch.First, batch.Count, &per_draw, 0,
				&TerrainField::GetInstanceLayout());
		}
	}

	void SubmitCapture(RenderView& view, Scene& scene, const glm::vec3& center, float radius,
		LayerMask culling_mask)
	{
		FillPrimaryLight(view);

		/* 探针六个面朝向不同，用不上相机视锥：按探针周围的一块范围选，LOD 也按它算 */
		std::vector<TerrainPatch> patches;
		patches.reserve(256);

		std::vector<TerrainBatch> batches;
		for (auto [entity, component] : scene.GetRegistry().view<TerrainComponent>().each())
		{
			if (component.GetMaterial() == nullptr)
				continue;
			if (!scene.IsEntityInView(entity, culling_mask))
				continue;

			if (component.m_Field == nullptr)
				component.m_Field = CreateSharedPtr<TerrainField>();
			component.m_Field->Rebuild(component, scene.GetRenderWorldTransform(entity));
			if (component.m_Field->IsEmpty())
				continue;

			const uint32_t first = static_cast<uint32_t>(patches.size());
			component.m_Field->BuildPatchesAround(center, radius, patches);
			if (patches.size() == first)
				continue;

			batches.push_back(TerrainBatch{ &component, component.m_Field.get(),
				static_cast<int>(entity), first, static_cast<uint32_t>(patches.size()) - first });
		}

		if (batches.empty())
			return;

		const Renderer::InstanceRange range = Renderer::UploadInstances(patches.data(),
			static_cast<uint32_t>(patches.size()), static_cast<uint32_t>(sizeof(TerrainPatch)));
		if (range.Buffer == nullptr)
			return;

		for (const TerrainBatch& batch : batches)
		{
			const SharedPtr<Material> source = batch.Component->GetMaterial();
			if (source == nullptr)
				continue;

			const SharedPtr<Material> material = GetForwardMaterial(source);
			if (material == nullptr)
				continue;

			DrawParams per_draw;
			BuildDrawParams(*batch.Field, *batch.Component, batch.EntityId, per_draw);

			Renderer::SubmitInstanced(material, TerrainField::GetPatchPrimitive(), range.Buffer,
				range.FirstInstance + batch.First, batch.Count, &per_draw, 0,
				&TerrainField::GetInstanceLayout());
		}
	}
}
