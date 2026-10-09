#include "Pch.h"
#include "CameraPreview.h"
#include "Helios/Renderer/RenderPasses/DeferredPasses.h"
#include "Helios/Renderer/RenderPasses/ScenePass.h"

namespace Helios
{
	CameraPreview::CameraPreview()
		: Camera()
	{
		PROFILE_FUNCTION();

		SetDebugName("CameraPreview");
	}

	void CameraPreview::SyncFrom(const Camera& source, const glm::mat4& world_transform,
		uint32_t width, uint32_t height)
	{
		PROFILE_FUNCTION();

		SetProjectionType(source.GetProjectionType());
		SetFov(source.GetFov());
		SetHeightSize(source.GetHeightSize());
		SetNearClip(source.GetNearClip());
		SetFarClip(source.GetFarClip());
		SetAspectRatio(source.GetAspectRatio());
		SetRenderPipeline(source.GetRenderPipeline());

		SetTransform(world_transform);
		GetRenderView()->SetViewportRegion({ 0, 0, width, height });

		/* 视图 / 投影矩阵：与来源相机在 Scene::OnUpdate 之后的状态同款 */
		OnUpdate(0.0f);
	}

	/* 按管线分派到视图纹理（与编辑器相机 ConstructRenderView 同一分派，只是没有叠加层）。
	 * 两种管线的输出都落在视图的 RenderTarget 上：前向 = ScenePassOutput，
	 * 延迟 = LightingPassOutput（由模块落成视图输出）。 */
	bool CameraPreview::ConstructRenderView(RenderView& render_view)
	{
		PROFILE_FUNCTION();

		/* 尺寸无效（尚未同步）：不组织渲染图。必须返回 true 表示"本相机负责组织"，
		 * 否则会回落到默认前向图并按 0 尺寸的渲染目标执行（同编辑器相机）。 */
		const auto& viewport_region = render_view.GetViewportRegion();
		if (viewport_region.Width == 0 || viewport_region.Height == 0)
			return true;

		if (render_view.GetRenderPipeline() == RenderPipeline::Deferred)
		{
			Deferred::AddDeferredPasses(&render_view);
		}
		else
		{
			render_view.AddShadowMapPasses();
			Forward::AddScenePassToTexture(&render_view);
		}

		render_view.Prepare();
		return true;
	}
}
