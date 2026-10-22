#pragma once
#include "Helios/Common/Export.h"
#include "Helios/Renderer/FrameGraph/FrameGraphResourceHandle.h"

namespace Helios
{
	class RenderView;
	struct FrameGraphTexture;

	namespace SelectionOutline
	{
		/* 选中实体描边：在视图输出上沿对象轮廓勾一圈强调色。
		 * object_id = 场景的实体 id 附件（延迟 GBuffer / 前向 ScenePass 各一张，都是 R32I），
		 * 选中 id 由视图持有（RenderView::SetSelectionId）。写在叠加层里、gizmo 之前，
		 * 被别的物体挡住的部分不描（id 冲突不到）。选空或句柄无效时是空操作。 */
		HELIOS_API void AddPass(RenderView& render_view,
			FrameGraphResourceHandleTyped<FrameGraphTexture> output,
			FrameGraphResourceHandleTyped<FrameGraphTexture> object_id);
	}
}
