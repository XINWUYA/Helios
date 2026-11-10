#pragma once
#include "Helios/Common/Export.h"
#include "Helios/Common/Common.h"
#include "Helios/Scene/LayerMask.h"
#include "Helios/Renderer/FrameGraph/FrameGraphResourceHandle.h"
#include "Helios/Renderer/RenderCommon.h"
#include <vector>

namespace Helios
{
	class RenderView;
	class Scene;
	class Material;
	struct FrameGraphTexture;
	struct VisibleMeshObject;
	namespace Forward
	{
		/* 提交批次：All = 本队列的全部对象（前向管线）；
		 * UnsupportedGBuffer = 只交装不进 G-Buffer 的那些（延迟管线的前向直画批）。 */
		enum class SubmitBatch
		{
			All,
			UnsupportedGBuffer,
		};

		/* 一次场景提交的输入：默认是"本视图的可见对象 + 视图相机位置 + 场景探针"。
		 * 探针烘焙借同一份提交，把自己按层收集的对象表和探针位置传进来。 */
		struct SubmitParams
		{
			SubmitBatch Batch{ SubmitBatch::All };
			DebugViewMode DebugView{ DebugViewMode::None };
			/* 对象表：为空时用视图的可见列表 */
			const std::vector<VisibleMeshObject>* Objects{ nullptr };
			/* 透明排序的参考点：为空时用视图相机位置 */
			const glm::vec3* SortOrigin{ nullptr };
			/* 捕获类提交：不采样场景探针的 IBL，否则会把上一次的烘焙结果烘进新结果 */
			bool IgnoreProbes{ false };
		};

		/* 前向场景 Pass（渲染到默认目标）：清屏后逐对象用材质各自的着色器直画，
		 * 含按对象选最近探针的 IBL per-draw 覆盖；阴影图取 Blackboard 的 "ShadowMapHandle"。 */
		void AddScenePass(RenderView* render_view);

		/* 前向场景 Pass（渲染到视图纹理）：输出颜色（RGBA8）+ ObjectId（R32I，按像素拾取）+ 深度
		 * 三张附件；颜色落 Blackboard 的 "ScenePassOutput"、深度落 "ScenePassDepth"，并设为视图输出。
		 * 编辑器视口的前向管线用它。 */
		HELIOS_API void AddScenePassToTexture(RenderView* render_view);

		/* 提交可见对象里指定批次（不透明 / 透明）到当前绑定的目标，逐光源一笔合成光照。
		 * 前向场景 Pass 与延迟管线的前向直画 / 透明 Pass 共用；调用方负责绑目标、视口与阴影纹理。
		 * debug_view 非 None 时材质着色器直接输出该档位的读数，接不上调试分流的换调试材质兜底 */
		void SubmitSceneObjects(RenderView& render_view, Scene& scene, bool transparent,
			SubmitBatch batch = SubmitBatch::All, DebugViewMode debug_view = DebugViewMode::None);

		/* 带输入的版本（探针烘焙用）：参数见 SubmitParams */
		void SubmitSceneObjects(RenderView& render_view, Scene& scene, bool transparent, const SubmitParams& params);

		/* 提交本视图里可见的草地：按块做视锥剔除、按距离抽稀后实例化绘制。
		 * 草不是实体、不在可见对象表里，所以单独走这一趟；用主方向光单笔着色
		 * （光照 UBO 一次只装一个光源，逐光源重画对几十万株草不可接受）。
		 * 调用方负责绑目标与阴影纹理，且已填好阴影光源。debug_view 同 SubmitSceneObjects */
		void SubmitGrassFields(RenderView& render_view, Scene& scene, DebugViewMode debug_view = DebugViewMode::None);

		/* 草只吃主方向光：光照 UBO 按主阴影光源填一份，可见着色与调试重绘共用 */
		void FillGrassLightUniform(RenderView& render_view);

		/* 草的调试重绘：与可见着色同一套提交（块底面 / 抽稀 / 间接），只换材质。
		 * 外观参数（底色、粗糙度）按每片草地的源材质走 per-draw 覆盖，
		 * 所以一件调试材质能服务整场草。调用方负责绑目标、深度与阴影纹理。 */
		void SubmitGrassDebug(RenderView& render_view, Scene& scene, const SharedPtr<Material>& material);

		/* 草的 G-Buffer 提交：与可见着色同一套剔除与实例缓冲，换成写 G-Buffer 的材质。
		 * 调用方负责绑目标（含深度）与视口。 */
		void SubmitGrassGBuffer(RenderView& render_view, Scene& scene);

		/* 场景里有没有本视图可见的草地（延迟管线据此决定挂不挂前向不透明那一趟） */
		bool HasVisibleGrassFields(const Scene& scene, LayerMask culling_mask);

		/* 运动矢量：把可见的不透明几何再画一遍，只输出屏幕空间位移（RG16F）。
		 * 深度用场景已经写好的那张（只测不写），结果落 Blackboard 的 "Velocity"。
		 * batch：All = 全部不透明对象（前向管线，会新建速度图并铺 0）；
		 *        UnsupportedGBuffer = 只补延迟管线里走前向直画的那批（草 + 装不进 G-Buffer 的材质），
		 *        此时必须给 target（G-Buffer 已经写过的那张），不清空、直接往上叠。 */
		void AddVelocityPass(RenderView* render_view, SubmitBatch batch = SubmitBatch::All,
			FrameGraphResourceHandleTyped<FrameGraphTexture> target = {});
	}
}
