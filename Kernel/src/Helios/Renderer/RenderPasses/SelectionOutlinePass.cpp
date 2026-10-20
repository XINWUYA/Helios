#include "Pch.h"
#include "SelectionOutlinePass.h"
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
		}

		void AddPass(RenderView& render_view,
			FrameGraphResourceHandleTyped<FrameGraphTexture> output,
			FrameGraphResourceHandleTyped<FrameGraphTexture> object_id)
		{
			PROFILE_FUNCTION();

			const int32_t selected_id = render_view.GetSelectionId();
			if (selected_id < 0 || !output || !object_id)
				return;

			auto& frame_graph = render_view.GetFrameGraph();
			const auto& viewport_region = render_view.GetViewportRegion();

			struct OutlinePassData
			{
				FrameGraphResourceHandleTyped<FrameGraphTexture> Output;
				FrameGraphResourceHandleTyped<FrameGraphTexture> ObjectId;
			};

			frame_graph->AddPass<OutlinePassData>("SelectionOutlinePass",
				[&](FrameGraphBuilder& builder, OutlinePassData& data)
				{
					data.Output = output;
					data.ObjectId = object_id;
					builder.BindInputResource(data.Output, FrameGraphTexture::Usage::ColorAttachment);
					builder.BindInputResource(data.ObjectId, FrameGraphTexture::Usage::Sampleable);

					FrameGraphPassInfo::Descriptor pass_desc;
					pass_desc.Attachments.ColorAttachments(0) = data.Output;
					/* 只往已有画面上叠一圈线，颜色原样保留 */
					pass_desc.PreserveContent = true;
					pass_desc.ViewportRegion = viewport_region;
					builder.CreateRenderPass("SelectionOutlineRenderTarget", pass_desc);

					/* 写进已有资源，不声明为目标会被图裁剪剔除（同 AxisPass） */
					builder.AsSideEffect();
				},
				[&render_view, selected_id](const FrameGraphResources& resources, const OutlinePassData& data)
				{
					const auto render_pass_info = resources.GetPassRenderTarget();
					render_view.EmplacePassFrameBuffer("SelectionOutlinePass", render_pass_info);

					const SharedPtr<Material> material = GetOutlineMaterial();
					material->SetTexture("u_ObjectId", resources.Get(data.ObjectId).Texture);
					material->SetParameters(ParamType::Int, "u_SelectedId", selected_id);

					render_pass_info->Bind();
					{
						Renderer::Submit(material, MeshPrimitive(Renderer::GetFullScreenVertexArray()));
					}
					render_pass_info->Unbind();
				});
		}
	}
}
