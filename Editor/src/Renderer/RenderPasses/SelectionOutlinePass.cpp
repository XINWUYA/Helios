#include "Pch.h"
#include "SelectionOutlinePass.h"
#include <algorithm>
#include <Helios/Core/AssetManager.h>
#include <Helios/Renderer/FrameGraph/FrameGraph.h>
#include <Helios/Renderer/Renderer.h>
#include <Helios/Renderer/RenderView.h>
#include <Helios/Scene/Material.h>
#include <Helios/Scene/Mesh.h>
#include <Helios/Scene/SceneCommon.h>
#include <Helios/VirtualDevice/DeviceShader.h>

namespace Helios
{
	namespace SelectionOutline
	{
		namespace
		{
			/* 描边宽度（物理像素）：贴着对象的一圈实心、外侧再淡出一像素，缩放屏上约两个逻辑像素。
			 * 颜色用操作手柄的高亮橙 #FF9E29，与工具栏 gizmo 的高亮同一套色 */
			constexpr float kThickness = 4.0f;
			const glm::vec4 kColor{ 1.000f, 0.620f, 0.160f, 1.0f };

			/* 粗筛掩码一块覆盖的边长（像素）：要比描边采样半径大，3x3 邻域才够盖住 ±半径 */
			constexpr int32_t kMaskTile = 8;

			/* 叠加材质只建一次（全屏直叠，参数不随对象变）：id 纹理每帧重设，
			 * 因为它来自本帧渲染图。 */
			SharedPtr<Material> GetOutlineMaterial()
			{
				static const SharedPtr<Material> s_Material = []()
				{
					auto material = CreateSharedPtr<Material>();
					material->SetShader(AssetManager::Instance().GetOrLoad<DeviceShader>(
						ABSOLUTE_PATH("Shaders/Overlay/SelectionOutline.glsl")));
					material->SetParameters(ParamType::Vec4, "u_OutlineColor", kColor);
					material->SetParameters(ParamType::Float, "u_Thickness", kThickness);
					material->SetParameters(ParamType::Int, "u_MaskTile", kMaskTile);

					/* 全屏直叠：不吃深度，按 alpha 混合；alpha 通道保持原值 */
					auto& raster_state = material->GetRasterState();
					raster_state.CullMode = CullMode::Cull_None;
					raster_state.EnableDepthTest = false;
					raster_state.EnableDepthWrite = false;
					raster_state.EnableBlend = true;
					raster_state.BlendFuncSrcRGB = BlendFunc::SrcAlpha;
					raster_state.BlendFuncDstRGB = BlendFunc::OneMinusSrcAlpha;
					raster_state.BlendFuncSrcA = BlendFunc::Zero;
					raster_state.BlendFuncDstA = BlendFunc::One;
					return material;
				}();
				return s_Material;
			}

			/* 粗筛掩码材质：把实体 id 附件按 tile 归约成"有 / 没选中像素"，纯写不混合 */
			SharedPtr<Material> GetMaskMaterial()
			{
				static const SharedPtr<Material> s_Material = []()
				{
					auto material = CreateSharedPtr<Material>();
					material->SetShader(AssetManager::Instance().GetOrLoad<DeviceShader>(
						ABSOLUTE_PATH("Shaders/Overlay/SelectionMask.glsl")));
					material->SetParameters(ParamType::Int, "u_MaskTile", kMaskTile);

					auto& raster_state = material->GetRasterState();
					raster_state.CullMode = CullMode::Cull_None;
					raster_state.EnableDepthTest = false;
					raster_state.EnableDepthWrite = false;
					raster_state.EnableBlend = false;
					return material;
				}();
				return s_Material;
			}
		}

		/* 实体在本视图里有没有可见几何：网格之外，草与地形也写进 G-Buffer。
		 * 新增非网格几何时在这里补一条 */
		bool HasVisibleGeometry(RenderView& render_view, int32_t selection_id)
		{
			const auto& visible_objects = render_view.GetVisibleMeshObjects();
			if (std::any_of(visible_objects.begin(), visible_objects.end(),
				[selection_id](const VisibleMeshObject& object) { return object.ObjectId == selection_id; }))
				return true;

			const auto scene = render_view.GetOwnerScene();
			if (scene == nullptr)
				return false;

			/* 实体 id 存的是 index，version 对不上，不能用 valid()，只能按 index 比 */
			const auto& registry = scene->GetRegistry();
			const auto same = [selection_id](auto entity)
			{
				return static_cast<int32_t>(static_cast<uint32_t>(entity)) == selection_id;
			};
			for (auto entity : registry.view<GrassComponent>())
			{
				if (same(entity)) return true;
			}
			for (auto entity : registry.view<TerrainComponent>())
			{
				if (same(entity)) return true;
			}
			return false;
		}

		void AddPass(RenderView& render_view, int32_t selection_id,
			FrameGraphResourceHandleTyped<FrameGraphTexture> output,
			FrameGraphResourceHandleTyped<FrameGraphTexture> object_id)
		{
			PROFILE_CPU_FUNCTION();

			if (selection_id < 0 || !output || !object_id)
				return;

			/* 选中项没有可见几何（相机 / 光源这类只有 gizmo 的节点）就整个跳过 */
			if (!HasVisibleGeometry(render_view, selection_id))
				return;

			auto& frame_graph = render_view.GetFrameGraph();
			const auto& viewport_region = render_view.GetViewportRegion();

			/* 粗筛掩码比视口小一圈：先花 1/64 的像素数标出"哪些 tile 里有选中像素"，
			 * 描边阶段绝大多数像素查几个掩码像素就能退出，不用逐像素扫 9x9 邻域 */
			const uint32_t mask_width = (viewport_region.Width + kMaskTile - 1) / kMaskTile;
			const uint32_t mask_height = (viewport_region.Height + kMaskTile - 1) / kMaskTile;

			struct MaskPassData
			{
				FrameGraphResourceHandleTyped<FrameGraphTexture> ObjectId;
				FrameGraphResourceHandleTyped<FrameGraphTexture> Mask;
			};

			const auto mask_pass = frame_graph->AddPass<MaskPassData>("SelectionOutlineMaskPass",
				[&](FrameGraphBuilder& builder, MaskPassData& data)
				{
					data.ObjectId = object_id;

					FrameGraphTexture::Descriptor mask_desc;
					mask_desc.Width = mask_width;
					mask_desc.Height = mask_height;
					mask_desc.TextureFormat = TextureFormat::R8;
					data.Mask = builder.CreateTexture("SelectionOutlineMask", mask_desc);

					builder.BindInputResource(data.ObjectId, FrameGraphTexture::Usage::Sampleable);
					builder.BindOutputResource(data.Mask,
						FrameGraphTexture::Usage::ColorAttachment | FrameGraphTexture::Usage::Sampleable);

					FrameGraphPassInfo::Descriptor pass_desc;
					pass_desc.Attachments.ColorAttachments(0) = data.Mask;
					pass_desc.ViewportRegion = ViewportRegion{ 0, 0, mask_width, mask_height };
					builder.CreateRenderPass("SelectionOutlineMaskRenderTarget", pass_desc);
				},
				[&render_view, selection_id](const FrameGraphResources& resources, const MaskPassData& data)
				{
					const auto render_pass_info = resources.GetPassRenderTarget();
					render_view.EmplacePassFrameBuffer("SelectionOutlineMaskPass", render_pass_info);

					const SharedPtr<Material> mask_material = GetMaskMaterial();
					mask_material->SetTexture("u_ObjectId", resources.Get(data.ObjectId).Texture);
					mask_material->SetParameters(ParamType::Int, "u_SelectedId", selection_id);

					render_pass_info->Bind();
					{
						Renderer::Submit(mask_material, MeshPrimitive(Renderer::GetFullScreenVertexArray()));
					}
					render_pass_info->Unbind();
				});

			struct OutlinePassData
			{
				FrameGraphResourceHandleTyped<FrameGraphTexture> Output;
				FrameGraphResourceHandleTyped<FrameGraphTexture> ObjectId;
				FrameGraphResourceHandleTyped<FrameGraphTexture> Mask;
			};

			frame_graph->AddPass<OutlinePassData>("SelectionOutlinePass",
				[&](FrameGraphBuilder& builder, OutlinePassData& data)
				{
					data.Output = output;
					data.ObjectId = object_id;
					data.Mask = mask_pass->GetData().Mask;
					builder.BindInputResource(data.Output, FrameGraphTexture::Usage::ColorAttachment);
					builder.BindInputResource(data.ObjectId, FrameGraphTexture::Usage::Sampleable);
					builder.BindInputResource(data.Mask, FrameGraphTexture::Usage::Sampleable);

					FrameGraphPassInfo::Descriptor pass_desc;
					pass_desc.Attachments.ColorAttachments(0) = data.Output;
					/* 只往已有画面上叠一圈线，颜色原样保留 */
					pass_desc.PreserveContent = true;
					pass_desc.ViewportRegion = viewport_region;
					builder.CreateRenderPass("SelectionOutlineRenderTarget", pass_desc);

					/* 写进已有资源，不声明为目标会被图裁剪剔除（同 AxisPass） */
					builder.AsSideEffect();
				},
				[&render_view, selection_id](const FrameGraphResources& resources, const OutlinePassData& data)
				{
					const auto render_pass_info = resources.GetPassRenderTarget();
					render_view.EmplacePassFrameBuffer("SelectionOutlinePass", render_pass_info);

					const SharedPtr<Material> material = GetOutlineMaterial();
					material->SetTexture("u_ObjectId", resources.Get(data.ObjectId).Texture);
					material->SetTexture("u_SelectionMask", resources.Get(data.Mask).Texture);
					material->SetParameters(ParamType::Int, "u_SelectedId", selection_id);

					render_pass_info->Bind();
					{
						Renderer::Submit(material, MeshPrimitive(Renderer::GetFullScreenVertexArray()));
					}
					render_pass_info->Unbind();
				});
		}
	}
}
