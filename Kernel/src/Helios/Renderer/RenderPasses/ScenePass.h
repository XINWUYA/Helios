#pragma once
#include "Helios/Common/Export.h"

namespace Helios
{
	class RenderView;
	class Scene;
	namespace Forward
	{
		/* 前向场景 Pass（渲染到默认目标）：清屏后逐对象用材质各自的着色器直画，
		 * 含按对象选最近探针的 IBL per-draw 覆盖；阴影图取 Blackboard 的 "ShadowMapHandle"。 */
		void AddScenePass(RenderView* render_view);

		/* 前向场景 Pass（渲染到视图纹理）：输出颜色（RGBA8）+ ObjectId（R32I，按像素拾取）+ 深度
		 * 三张附件；颜色落 Blackboard 的 "ScenePassOutput"、深度落 "ScenePassDepth"，并设为视图输出。
		 * 编辑器视口的前向管线用它。 */
		HELIOS_API void AddScenePassToTexture(RenderView* render_view);

		/* 提交可见对象里指定批次（不透明 / 透明）到当前绑定的目标，逐光源一笔合成光照。
		 * 前向场景 Pass 与延迟管线的透明 Pass 共用；调用方负责绑目标、视口与阴影纹理。 */
		void SubmitSceneObjects(RenderView& render_view, Scene& scene, bool transparent);
	}
}
