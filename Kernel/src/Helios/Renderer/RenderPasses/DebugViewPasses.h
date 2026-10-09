#pragma once
#include "Helios/Renderer/FrameGraph/FrameGraphResourceHandle.h"

namespace Helios
{
	class RenderView;
	struct FrameGraphTexture;

	namespace DebugView
	{
		/* 调试视图的写入口：Output = 视图输出（合成 / 几何重绘的落点）。
		 * HasGBuffer：延迟 = true（Surface 类从 G-Buffer 直读，读数即光照实际输入）；
		 * 前向 = false（无 G-Buffer 可读，Surface 类退化为材质贴图重采样近似）。 */
		struct Target
		{
			FrameGraphResourceHandleTyped<FrameGraphTexture> Output;
			bool HasGBuffer{ false };
		};

		/* 按视图的调试模式追加对应的调试 Pass。调用时机 = 管线主体之后、天空背景之前（合成和几何
		 * 重绘先落笔，天空最后收背景）；模式为 None 或由光照阶段分流时是空操作。 */
		void AddDebugViewPasses(RenderView& render_view, const Target& target);
	}
}
