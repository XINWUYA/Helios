#pragma once
#include "Helios/Common/Common.h"
#include "Helios/Renderer/FrameGraph/FrameGraphResourceHandle.h"
#include "Helios/Renderer/RenderCommon.h"

namespace Helios
{
	class Material;
	class RenderView;
	struct FrameGraphTexture;

	namespace DebugView
	{
		/* 调试视图的写入口：Output = 视图输出（合成 / 几何重绘的落点）。
		 * HasGBuffer：延迟 = true（Surface 类从 G-Buffer 直读，读数即光照实际输入）；
		 * 前向 = false（Surface 与光照分量由材质着色器在场景 Pass 里自己输出）。 */
		struct Target
		{
			FrameGraphResourceHandleTyped<FrameGraphTexture> Output;
			bool HasGBuffer{ false };
		};

		/* 按视图的调试模式追加对应的调试 Pass。调用时机 = 管线主体之后、天空背景之前（合成和几何
		 * 重绘先落笔，天空最后收背景）；模式为 None、由光照阶段分流、或由编辑器叠加层绘制
		 * （Skeleton）时是空操作。 */
		void AddDebugViewPasses(RenderView& render_view, const Target& target);

		/* 前向场景 Pass 交给材质着色器自己出读数的档位（Surface 与光照分量），其余返回 None。
		 * Shadow 要多光源求平均，另走累加 Pass，不在这里。 */
		DebugViewMode GetForwardMaterialMode(DebugViewMode mode);

		/* 前向调试的兜底材质：没接调试分流的自定义着色器换成 DebugMaterial.glsl 重画，
		 * Surface 档采源材质对应的那张贴图，光照分量画黑 */
		SharedPtr<Material> CreateFallbackMaterial(const SharedPtr<Material>& source, DebugViewMode mode);
	}
}
