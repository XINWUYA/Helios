#pragma once
#include <algorithm>
#include <glm/glm.hpp>
#include <vector>

#include "Helios/Renderer/RenderView.h"
#include "Helios/Scene/Material.h"

namespace Helios::RenderSorting
{
	/* 天空盒恒走不透明批：它铺满背景、深度落在远平面，进透明批会被当成一块
	 * 半透明片盖住场景。其余对象按材质的渲染队列判。 */
	inline bool IsTransparentObject(const VisibleMeshObject& object)
	{
		if (object.Material == nullptr || object.Material->IsSkyBox())
			return false;
		return object.Material->IsTransparent();
	}

	/* 可见对象里有没有透明物体：延迟管线据此决定要不要挂前向透明 Pass
	 * （没有透明物体的场景不该多一趟附件切换）。 */
	inline bool HasTransparentObjects(const std::vector<VisibleMeshObject>& objects)
	{
		return std::any_of(objects.begin(), objects.end(),
			[](const VisibleMeshObject& object) { return IsTransparentObject(object); });
	}

	/* 透明对象的光栅状态：深度测试保留、写入关掉 —— 透明层之间才能叠起来，
	 * 同时仍被前面的不透明物体挡住。材质没配混合（默认 One / Zero 等价于覆盖）
	 * 时按常规半透明画；配过就沿用（加法特效之类照旧）。 */
	inline RenderRasterState MakeTransparentRasterState(const RenderRasterState& base)
	{
		RenderRasterState state = base;
		state.EnableDepthTest = true;
		state.EnableDepthWrite = false;

		const bool default_blend =
			state.BlendFuncSrcRGB == BlendFunc::One && state.BlendFuncDstRGB == BlendFunc::Zero &&
			state.BlendFuncSrcA == BlendFunc::One && state.BlendFuncDstA == BlendFunc::Zero;
		if (!state.EnableBlend || default_blend)
		{
			state.EnableBlend = true;
			state.BlendEquationRGB = BlendEquation::Add;
			state.BlendEquationA = BlendEquation::Add;
			state.BlendFuncSrcRGB = BlendFunc::SrcAlpha;
			state.BlendFuncSrcA = BlendFunc::SrcAlpha;
			state.BlendFuncDstRGB = BlendFunc::OneMinusSrcAlpha;
			state.BlendFuncDstA = BlendFunc::OneMinusSrcAlpha;
		}
		return state;
	}

	/* 收集一批要绘制的对象并排序：
	 * 不透明 -> 队列升序，同队列保持场景里的收集顺序（稳定排序，不抖）；
	 * 透明   -> 队列升序，同队列按到相机的距离从远到近（近的后画，叠得对）。
	 * 材质为空的对象两个批次都不收。 */
	inline std::vector<const VisibleMeshObject*> CollectDrawList(
		const std::vector<VisibleMeshObject>& objects, bool transparent, const glm::vec3& view_position)
	{
		std::vector<const VisibleMeshObject*> draw_list;
		draw_list.reserve(objects.size());

		for (const VisibleMeshObject& object : objects)
		{
			if (object.Material == nullptr)
				continue;
			if (IsTransparentObject(object) != transparent)
				continue;
			draw_list.push_back(&object);
		}

		/* 距离用世界位置到相机的平方距离：只比大小，不用开方 */
		const auto distance_sq = [view_position](const VisibleMeshObject* object)
		{
			const glm::vec3 position(object->Local2WorldMat[3]);
			return glm::dot(position - view_position, position - view_position);
		};

		std::stable_sort(draw_list.begin(), draw_list.end(),
			[transparent, &distance_sq](const VisibleMeshObject* lhs, const VisibleMeshObject* rhs)
			{
				const int32_t lhs_queue = lhs->Material->GetRenderQueue();
				const int32_t rhs_queue = rhs->Material->GetRenderQueue();
				if (lhs_queue != rhs_queue)
					return lhs_queue < rhs_queue;
				if (!transparent)
					return false;
				return distance_sq(lhs) > distance_sq(rhs);
			});

		return draw_list;
	}
}
