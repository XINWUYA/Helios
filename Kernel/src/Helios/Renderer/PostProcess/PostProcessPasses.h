#pragma once
#include "PostProcessEffectRegistry.h"

namespace Helios
{
	namespace PostProcess
	{
		/* 本视图是否存在会实际生效的后处理（任一效果启用；调试视图恒 false） */
		bool HasEnabledEffects(const RenderView& render_view);

		/* 后处理通用入口：在管线主体之后调用，返回视图最终颜色句柄。
		 * 调试视图直接跳过；否则沿效果注册链逐环套用。 */
		FrameGraphResourceHandleTyped<FrameGraphTexture> AddPostProcessPasses(
			RenderView& render_view, FrameGraphResourceHandleTyped<FrameGraphTexture> source);

		/* 默认目标路径的收尾入口：把（已渲染到纹理的）source 全屏呈现回默认目标。
		 * 调用前提：HasEnabledEffects 为真且场景已旁路到纹理。 */
		void AddPresentToDefaultTarget(
			RenderView& render_view, FrameGraphResourceHandleTyped<FrameGraphTexture> source);
	}
}
