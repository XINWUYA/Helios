#pragma once
#include <cstdint>
#include "Helios/Renderer/FrameGraph/FrameGraphResourceHandle.h"

namespace Helios
{
	class RenderView;
	struct FrameGraphTexture;

	namespace SelectionOutline
	{
		/* 选中实体描边：在视图输出上沿对象轮廓勾一圈强调色。
		 * object_id = 场景的实体 id 附件（延迟 GBuffer / 前向 ScenePass 各一张，都是 R32I），
		 * selection_id 由编辑器相机给，负数 = 没有选中、直接跳过。写在叠加层里、gizmo 之前，
		 * 被别的物体挡住的部分不描（id 冲突不到）。 */
		void AddPass(RenderView& render_view, int32_t selection_id,
			FrameGraphResourceHandleTyped<FrameGraphTexture> output,
			FrameGraphResourceHandleTyped<FrameGraphTexture> object_id);
	}
}
