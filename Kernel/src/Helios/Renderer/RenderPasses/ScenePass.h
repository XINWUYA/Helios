#pragma once

namespace Helios
{
	class RenderView;
	namespace Forward
	{
		/* 前向场景 Pass（渲染到默认目标）：清屏后逐对象用材质各自的着色器直画，
		 * 含按对象选最近探针的 IBL per-draw 覆盖；阴影图取 Blackboard 的 "ShadowMapHandle"。 */
		void AddScenePass(RenderView* render_view);

		/* 前向场景 Pass（渲染到视图纹理）：输出颜色（RGBA8）+ ObjectId（R32I，按像素拾取）+ 深度
		 * 三张附件；颜色落 Blackboard 的 "ScenePassOutput"、深度落 "ScenePassDepth"，并设为视图输出。
		 * 编辑器视口的前向管线用它。 */
		void AddScenePassToTexture(RenderView* render_view);
	}
}
