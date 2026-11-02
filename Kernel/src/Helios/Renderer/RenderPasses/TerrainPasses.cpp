#include "Pch.h"
#include "TerrainPasses.h"

#include <Helios/Core/AssetManager.h>
#include <Helios/Renderer/Frustum.h>
#include <Helios/Renderer/Renderer.h>
#include <Helios/Renderer/RenderView.h>
#include <Helios/Scene/Camera.h>
#include <Helios/Scene/Components.h>
#include <Helios/Scene/Material.h>
#include <Helios/Scene/Scene.h>
#include <Helios/Scene/SceneCommon.h>
#include <Helios/Scene/ShadowMap.h>
#include <Helios/Scene/TerrainField.h>
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

		/* 逐地形参数：高度图与覆盖范围走 per-draw 覆盖，材质保持共享 */
		void BuildDrawParams(const TerrainField& field, const TerrainComponent& component, DrawParams& per_draw)
		{
			per_draw.FillMissing = MaterialFill::Values | MaterialFill::Textures;
			per_draw.Overrides.clear();
			per_draw.Overrides.push_back(Material::MakeTextureParam("u_HeightMap", field.GetHeightMap()));
			per_draw.Overrides.emplace_back(ParamType::Vec3, "u_TerrainOrigin", field.GetOrigin());
			per_draw.Overrides.emplace_back(ParamType::Vec2, "u_TerrainExtent", field.GetExtent());
			per_draw.Overrides.emplace_back(ParamType::Vec2, "u_HeightmapSize",
				glm::vec2(static_cast<float>(field.GetResolution())));
			per_draw.Overrides.emplace_back(ParamType::Float, "u_HeightScale", field.GetHeightScale());
			per_draw.Overrides.emplace_back(ParamType::Float, "u_TerrainReceiveShadow",
				component.m_ReceiveShadow ? 1.0f : 0.0f);
		}

		/* 收集本视图可见的地形、生成 patch、写进本帧实例缓冲 */
		bool BuildBatches(RenderView& view, Scene& scene, std::vector<TerrainBatch>& batches,
			Renderer::InstanceRange& range)
		{
			const Camera* camera = view.GetCullingCamera();
			if (camera == nullptr)
				return false;

			const glm::vec3 eye = camera->GetPosition();
			const Frustum frustum = Frustum::FromViewProjection(camera->GetViewProjectionMatrix());

			std::vector<TerrainPatch> patches;
			patches.reserve(512);

			for (auto [entity, component] : scene.GetRegistry().view<TerrainComponent>().each())
			{
				if (component.GetMaterial() == nullptr)
					continue;
				if (!scene.IsEntityInRenderView(entity, view.GetCullingMask()))
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

		void SubmitBatches(RenderView& view, Scene& scene, bool forward)
		{
			PROFILE_FUNCTION();

			std::vector<TerrainBatch> batches;
			batches.reserve(4);

			Renderer::InstanceRange range;
			if (!BuildBatches(view, scene, batches, range))
				return;

			for (const TerrainBatch& batch : batches)
			{
				if (batch.Component == nullptr || batch.Field == nullptr)
					continue;

				const SharedPtr<Material> source = batch.Component->GetMaterial();
				const SharedPtr<Material> material = forward ? GetForwardMaterial(source) : source;
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

	bool HasVisibleTerrain(const Scene& scene, LayerMask culling_mask)
	{
		for (auto [entity, component] : scene.GetRegistry().view<TerrainComponent>().each())
		{
			if (component.GetMaterial() == nullptr)
				continue;
			if (scene.IsEntityInRenderView(entity, culling_mask))
				return true;
		}
		return false;
	}

	void SubmitGBuffer(RenderView& view, Scene& scene)
	{
		SubmitBatches(view, scene, false);
	}

	void SubmitForward(RenderView& view, Scene& scene)
	{
		/* 地形只吃主方向光：光照 UBO 先填成主阴影光源，与草地同一份口径 */
		const auto& lights = view.GetValidLights();
		if (lights.empty())
		{
			Renderer::FillEmptyLightUniformBuffer();
		}
		else
		{
			const ShadowMapManager* shadow_maps = view.GetShadowMapManager().get();
			const size_t primary = (shadow_maps != nullptr)
				? shadow_maps->FindPrimaryShadowLightIndex(lights) : 0;
			Renderer::FillLightUniformBuffer(lights[std::min(primary, lights.size() - 1)], shadow_maps);
		}

		SubmitBatches(view, scene, true);
	}
}
