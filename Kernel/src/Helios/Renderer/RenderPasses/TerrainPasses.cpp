#include "Pch.h"
#include "TerrainPasses.h"

#include <Helios/Core/AssetManager.h>
#include <Helios/Renderer/Frustum.h>
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
			uint32_t First{ 0 };
			uint32_t Count{ 0 };
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

			/* 没见过这个视图：顶掉最旧的一行 */
			FrameCache& slot = (s_Caches[0].Frame <= s_Caches[1].Frame) ? s_Caches[0] : s_Caches[1];
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

		/* 前向材质：从组件的材质克隆一份换 shader。两个 shader 的参数名一致，
		 * 换完直接沿用；源材质参数版本变了才重建 */
		SharedPtr<Material> GetForwardMaterial(const SharedPtr<Material>& source)
		{
			struct Entry
			{
				SharedPtr<Material> Material;
				uint32_t Revision{ 0 };
			};
			static std::unordered_map<const Material*, Entry> s_Cache;

			Entry& entry = s_Cache[source.get()];
			if (entry.Material != nullptr && entry.Revision == source->GetRevision())
				return entry.Material;

			const auto shader = AssetManager::Instance().GetOrLoad<DeviceShader>(
				ABSOLUTE_PATH("Shaders/MaterialShaders/TerrainForward.glsl"));
			if (shader == nullptr)
				return source;

			auto material = source->Clone();
			material->SetShader(shader);
			entry.Material = material;
			entry.Revision = source->GetRevision();
			return material;
		}

		/* 高度图与覆盖范围：材质本身多块地形共用，逐地形的差异全走 per-draw 覆盖 */
		void AddTerrainOverrides(const TerrainField& field, DrawParams& per_draw)
		{
			per_draw.FillMissing = MaterialFill::Values | MaterialFill::Textures;
			per_draw.Overrides.clear();
			per_draw.Overrides.push_back(Material::MakeTextureParam("u_HeightMap", field.GetHeightMap()));
			per_draw.Overrides.emplace_back(ParamType::Vec3, "u_TerrainOrigin", field.GetOrigin());
			per_draw.Overrides.emplace_back(ParamType::Vec2, "u_TerrainExtent", field.GetExtent());
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
		void AddVirtualTextureParams(const TerrainVirtualTexture& virtual_texture, DrawParams& per_draw)
		{
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
		}

		/* 可见着色的逐绘制参数：是否接收阴影 + 虚拟纹理。只有声明了这些参数的着色器才能用，
		 * 名字反射不到时 ApplyParam 会回退到槽位 0，把高度图挤掉 */
		void BuildDrawParams(const TerrainField& field, const TerrainComponent& component, DrawParams& per_draw)
		{
			AddTerrainOverrides(field, per_draw);
			per_draw.Overrides.emplace_back(ParamType::Float, "u_TerrainReceiveShadow",
				component.m_ReceiveShadow ? 1.0f : 0.0f);
			AddVirtualTextureParams(field.GetVirtualTexture(), per_draw);
		}

		/* 按给定视锥选 patch 并上传，返回本帧实例环里的位置。
		 * batches 记录每块地形占的区间，调用方据此逐块提交 */
		bool BuildAndUpload(Scene& scene, LayerMask culling_mask, const Frustum& frustum, const glm::vec3& eye,
			std::vector<TerrainPatch>& patches, std::vector<TerrainBatch>& batches,
			Renderer::InstanceRange& range)
		{
			patches.clear();
			batches.clear();

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

				const uint32_t first = static_cast<uint32_t>(patches.size());
				component.m_Field->BuildPatches(frustum, eye, patches);
				if (patches.size() == first)
					continue;

				batches.push_back(TerrainBatch{ &component, component.m_Field.get(), first,
					static_cast<uint32_t>(patches.size()) - first });
			}

			if (batches.empty())
				return false;


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
				cache.Patches, cache.Batches, cache.Range);
			return cache;
		}

		void SubmitVisibleBatch(const FrameCache& frame, bool forward, DebugViewMode debug_view)
		{
			if (frame.Range.Buffer == nullptr)
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
				BuildDrawParams(*batch.Field, *batch.Component, per_draw);

				/* 前向的读数由材质自己出；延迟的 Surface / Shadow 档读 G-Buffer，不用管 */
				if (forward && debug_view != DebugViewMode::None)
					per_draw.Overrides.emplace_back(ParamType::Int, "u_DebugView",
						static_cast<int>(debug_view));

				Renderer::SubmitInstanced(final_material, TerrainField::GetPatchPrimitive(), frame.Range.Buffer,
					frame.Range.FirstInstance + batch.First, batch.Count, &per_draw, 0,
					&TerrainField::GetInstanceLayout());
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

		/* 源材质的数值参数整份抄进 per-draw：页烘焙要跟可见着色用同一套外观 */
		void CopyNumericParams(const Material& source, DrawParams& per_draw)
		{
			for (const auto& [key, info] : source.GetAllParameters())
			{
				(void)key;
				if (info.Type == ParamType::Texture)
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

	void SubmitGBuffer(RenderView& view, Scene& scene)
	{
		const FrameCache& frame = EnsureVisibleFrame(view, scene);
		SubmitVisibleBatch(frame, false, DebugViewMode::None);
	}

	void BakeVirtualTextures(RenderView& view, Scene& scene)
	{
		if (!TerrainVirtualTexture::IsEnabled())
			return;

		const Camera* camera = view.GetCullingCamera();
		if (camera == nullptr)
			return;

		const SharedPtr<Material> material = GetVirtualTextureBakeMaterial();
		if (material == nullptr)
			return;

		const FrameCache& frame = EnsureVisibleFrame(view, scene);
		if (frame.Batches.empty() || frame.Patches.empty())
			return;

		const glm::vec3 eye = camera->GetPosition();
		for (const TerrainBatch& batch : frame.Batches)
		{
			if (batch.Component == nullptr || batch.Field == nullptr || batch.Count == 0)
				continue;

			TerrainVirtualTexture& virtual_texture = batch.Component->m_Field->GetVirtualTexture();
			if (!virtual_texture.EnsureResources())
				continue;

			virtual_texture.Prepare(*batch.Field, frame.Patches.data() + batch.First, batch.Count, eye);
			if (!virtual_texture.HasPending())
				continue;

			const SharedPtr<DeviceFrameBuffer>& frame_buffer = virtual_texture.GetBakeFrameBuffer();
			if (frame_buffer == nullptr)
				continue;

			const glm::vec3 origin = batch.Field->GetOrigin();
			const glm::vec2 extent = batch.Field->GetExtent();
			const glm::vec2 min_corner(origin.x - extent.x, origin.z - extent.y);

			for (const glm::uvec3& page : virtual_texture.GetPending())
			{
				/* 页池的一层当颜色附件：一次只写一层，烘完解绑，主通道照着自己绑 */
				frame_buffer->Bind(FrameBufferBindInfo::ToColorLayer(0, static_cast<uint16_t>(page.z), 0));

				DrawParams per_draw;
				AddTerrainOverrides(*batch.Field, per_draw);
				if (batch.Component->GetMaterial() != nullptr)
					CopyNumericParams(*batch.Component->GetMaterial(), per_draw);

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
		if (frame.Range.Buffer == nullptr || material == nullptr)
			return;

		for (const TerrainBatch& batch : frame.Batches)
		{
			const SharedPtr<Material> source = batch.Component->GetMaterial();
			if (source == nullptr)
				continue;

			DrawParams per_draw;
			AddTerrainOverrides(*batch.Field, per_draw);
			AddVirtualTextureParams(batch.Field->GetVirtualTexture(), per_draw);

			/* 外观参数从源材质抄过来，读数才跟正常着色一致 */
			for (const auto& [key, info] : source->GetAllParameters())
			{
				(void)key;
				if (info.Type == ParamType::Texture)
					continue;
				per_draw.Overrides.push_back(info);
			}

			Renderer::SubmitInstanced(material, TerrainField::GetPatchPrimitive(), frame.Range.Buffer,
				frame.Range.FirstInstance + batch.First, batch.Count, &per_draw, 0,
				&TerrainField::GetInstanceLayout());
		}
	}

	void SubmitVelocity(RenderView& view, Scene& scene)
	{
		const SharedPtr<Material> material = GetVelocityMaterial();
		if (material == nullptr)
			return;

		const FrameCache& frame = EnsureVisibleFrame(view, scene);
		if (frame.Range.Buffer == nullptr)
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

			Renderer::SubmitInstanced(material, TerrainField::GetPatchPrimitive(), frame.Range.Buffer,
				frame.Range.FirstInstance + batch.First, batch.Count, &per_draw, 0,
				&TerrainField::GetInstanceLayout());
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
		if (!BuildAndUpload(scene, view.GetCullingMask(), frustum, camera->GetPosition(),
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

	void SubmitCapture(RenderView& view, Scene& scene, const glm::vec3& center, float radius)
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
			if (!scene.IsEntityInView(entity, view.GetCullingMask()))
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

			batches.push_back(TerrainBatch{ &component, component.m_Field.get(), first,
				static_cast<uint32_t>(patches.size()) - first });
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
			BuildDrawParams(*batch.Field, *batch.Component, per_draw);

			Renderer::SubmitInstanced(material, TerrainField::GetPatchPrimitive(), range.Buffer,
				range.FirstInstance + batch.First, batch.Count, &per_draw, 0,
				&TerrainField::GetInstanceLayout());
		}
	}
}
