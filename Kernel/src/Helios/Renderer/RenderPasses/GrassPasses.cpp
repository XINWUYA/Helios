#include "Pch.h"
#include "GrassPasses.h"

#include <Helios/Renderer/FrameGraph/FrameGraph.h>
#include <Helios/Renderer/RenderView.h>
#include <Helios/Scene/Components.h>
#include <Helios/Scene/GrassField.h>
#include <Helios/Scene/Scene.h>

namespace Helios::GrassDraw
{
	void AddIndirectCullPass(RenderView& view)
	{
		if (!GrassField::IsGlobalIndirectDrawEnabled())
			return;

		const SharedPtr<Scene> scene = view.GetOwnerScene();
		if (scene == nullptr || view.GetCullingCamera() == nullptr)
			return;

		/* 场景里没有可见的草就别占一个 Pass */
		bool has_grass = false;
		for (auto [entity, grass] : scene->GetRegistry().view<GrassComponent>().each())
		{
			if (grass.GetMaterial() == nullptr || !scene->IsEntityInView(entity, view.GetCullingMask()))
				continue;
			if (grass.m_Field == nullptr || grass.m_Field->IsEmpty())
				continue;
			has_grass = true;
			break;
		}
		if (!has_grass)
			return;

		struct CullPassData
		{
		};

		view.GetFrameGraph()->AddPass<CullPassData>("GrassIndirectCullPass",
			[](FrameGraphBuilder& builder, CullPassData&)
			{
				/* 结果写在 GrassField 自己的槽位缓冲里，图里暂时没有可绑的资源；
				 * 计算 Pass 不绑默认渲染目标 */
				builder.AsSideEffect();
				builder.AsComputePass();
			},
			[&view, scene](const FrameGraphResources&, const CullPassData&)
			{
				/* 级联矩阵在这之前已经定稿，各档视锥取得到 */
				scene->PrepareGrassIndirect(view);
			});
	}
}
