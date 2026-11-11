#pragma once
#include "Helios/Renderer/FrameGraph/FrameGraphResourceHandle.h"

namespace Helios
{
	class RenderView;
	template<typename ResourceType>
	class FrameGraphResourceHandleTyped;
	class FrameGraphTexture;

	namespace UI
	{
		/* 视图合成：把管线输出拷进一张新的视图纹理，再在上面画运行时 UI。
		 * 不原地写输入 —— 关掉后处理时输入就是下一帧要读的历史纹理，UI 进去会拖影（D5）。
		 * 视图没开 UI 或场景里没有画布时原样返回输入，不建 Pass。
		 * 结果同时写进 Blackboard 的 "ViewOutput"，编辑器叠加层从那里取。 */
		FrameGraphResourceHandleTyped<FrameGraphTexture> AddCompositePass(RenderView& view,
			FrameGraphResourceHandleTyped<FrameGraphTexture> source);
	}
}
