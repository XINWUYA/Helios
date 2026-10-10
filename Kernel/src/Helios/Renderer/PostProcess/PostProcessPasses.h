#pragma once
#include "PostProcessEffectRegistry.h"

namespace Helios
{
	namespace PostProcess
	{
		/* 后处理通用入口（框架）：在管线主体（含天空 / 调试合成）之后调用，返回视图最终颜色句柄。
		 * 调试视图非 None 时直通；否则沿效果注册链逐环套用（未启用 / 不适用的效果由自身直通）。
		 * 管线接入点（DeferredPasses / ScenePassToTexture）只用这一个入口，不随效果增删改动。 */
		FrameGraphResourceHandleTyped<FrameGraphTexture> AddPostProcessPasses(
			RenderView& render_view, FrameGraphResourceHandleTyped<FrameGraphTexture> source);

		/* 本视图是否存在会实际生效的后处理（快照里有任一启用的效果；调试视图非 None 恒 false）。
		 * 前向默认目标路径据此决定：是否需要"旁路到纹理再呈现"（false = 照旧直渲，零改动）。 */
		bool HasEnabledEffects(const RenderView& render_view);

		/* 默认目标路径的收尾入口：把（已渲染到纹理的）source 全屏呈现回默认目标
		 * （无附件 Pass，落窗口画布）。调用前提：HasEnabledEffects 为真且场景已旁路到纹理。 */
		void AddPresentToDefaultTarget(
			RenderView& render_view, FrameGraphResourceHandleTyped<FrameGraphTexture> source);
	}
}
