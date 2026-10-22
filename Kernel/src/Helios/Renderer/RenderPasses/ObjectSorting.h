#pragma once
#include <algorithm>
#include <glm/glm.hpp>
#include <vector>

#include "Helios/Renderer/RenderView.h"
#include "Helios/Scene/Material.h"
#include "Helios/Scene/Mesh.h"
#include "Helios/VirtualDevice/DeviceVertexArray.h"

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

	/* 绘制状态键：程序、光栅状态、顶点布局这三样决定一次重绑要花多少。
	 * 同键的对象连着画，管线切换和材质重绑就从"每个对象"摊到"每组一次"。 */
	inline uint64_t MakeDrawStateKey(const Material& material, const DeviceVertexArray* vertex_array)
	{
		const RenderRasterState& state = material.GetRasterState();

		uint64_t key = 0xCBF29CE484222325ULL;
		const auto mix = [&key](uint64_t value)
		{
			key ^= value;
			key *= 0x100000001B3ULL;
		};

		mix(reinterpret_cast<uintptr_t>(material.GetShader().get()));
		mix(reinterpret_cast<uintptr_t>(vertex_array));
		mix(static_cast<uint64_t>(state.CullMode));
		mix(static_cast<uint64_t>(state.FrontFaceType));
		mix(state.EnableBlend ? 1u : 0u);
		mix(static_cast<uint64_t>(state.BlendEquationRGB));
		mix(static_cast<uint64_t>(state.BlendEquationA));
		mix(static_cast<uint64_t>(state.BlendFuncSrcRGB));
		mix(static_cast<uint64_t>(state.BlendFuncSrcA));
		mix(static_cast<uint64_t>(state.BlendFuncDstRGB));
		mix(static_cast<uint64_t>(state.BlendFuncDstA));
		mix(state.EnableDepthTest ? 1u : 0u);
		mix(state.EnableDepthWrite ? 1u : 0u);
		mix(static_cast<uint64_t>(state.DepthCompareFunc));
		mix(state.EnableColorWrite ? 1u : 0u);
		mix(state.Wireframe ? 1u : 0u);
		return key;
	}

	/* 收集一批要绘制的对象并排序：
	 * 不透明 -> 队列升序，同队列按状态键分组（同材质同布局连着画），组内保持收集顺序；
	 * 透明   -> 队列升序，同队列按到相机的距离从远到近（近的后画，叠得对）。
	 * 透明批不能按状态重排，否则混合顺序就乱了。材质为空的对象两个批次都不收。 */
	inline std::vector<const VisibleMeshObject*> CollectDrawList(
		const std::vector<VisibleMeshObject>& objects, bool transparent, const glm::vec3& view_position)
	{
		struct DrawEntry
		{
			int32_t Queue{ 0 };
			uint64_t StateKey{ 0 };
			float DistanceSq{ 0.0f };
			const VisibleMeshObject* Object{ nullptr };
		};

		std::vector<DrawEntry> entries;
		entries.reserve(objects.size());

		for (const VisibleMeshObject& object : objects)
		{
			if (object.Material == nullptr)
				continue;
			if (IsTransparentObject(object) != transparent)
				continue;

			DrawEntry entry;
			entry.Queue = object.Material->GetRenderQueue();
			entry.Object = &object;

			if (transparent)
			{
				/* 距离用世界位置到相机的平方距离：只比大小，不用开方 */
				const glm::vec3 position(object.Local2WorldMat[3]);
				entry.DistanceSq = glm::dot(position - view_position, position - view_position);
			}
			else
			{
				const auto* vertex_array = (object.MeshSegment != nullptr)
					? object.MeshSegment->GetMeshPrimitive().VertexArray.get() : nullptr;
				entry.StateKey = MakeDrawStateKey(*object.Material, vertex_array);
			}

			entries.push_back(entry);
		}

		std::stable_sort(entries.begin(), entries.end(),
			[transparent](const DrawEntry& lhs, const DrawEntry& rhs)
			{
				if (lhs.Queue != rhs.Queue)
					return lhs.Queue < rhs.Queue;
				if (transparent)
					return lhs.DistanceSq > rhs.DistanceSq;
				return lhs.StateKey < rhs.StateKey;
			});

		std::vector<const VisibleMeshObject*> draw_list;
		draw_list.reserve(entries.size());
		for (const DrawEntry& entry : entries)
			draw_list.push_back(entry.Object);
		return draw_list;
	}

	/* G-Buffer 批：按材质 + 网格分组。
	 * 天空盒不进 G-Buffer —— 它输出最终颜色、顶点着色器把网格折成全屏天空，转成 G-Buffer
	 * 材质只会得到一颗实心球；透明物体也不进 —— G-Buffer 只存单层表面，叠不出半透明。
	 * 这些材质最后都会转成同一套 G-Buffer 着色器、光栅状态也统一，所以只按材质和网格排。 */
	inline std::vector<const VisibleMeshObject*> CollectGBufferList(
		const std::vector<VisibleMeshObject>& objects)
	{
		std::vector<std::pair<uint64_t, const VisibleMeshObject*>> entries;
		entries.reserve(objects.size());

		for (const VisibleMeshObject& object : objects)
		{
			if (object.Material == nullptr || object.MeshSegment == nullptr)
				continue;
			if (object.Material->IsSkyBox() || object.Material->IsTransparent())
				continue;

			uint64_t key = 0xCBF29CE484222325ULL;
			const auto mix = [&key](uint64_t value)
			{
				key ^= value;
				key *= 0x100000001B3ULL;
			};
			mix(reinterpret_cast<uintptr_t>(object.Material.get()));
			mix(reinterpret_cast<uintptr_t>(object.MeshSegment->GetMeshPrimitive().VertexArray.get()));

			entries.emplace_back(key, &object);
		}

		std::stable_sort(entries.begin(), entries.end(),
			[](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

		std::vector<const VisibleMeshObject*> draw_list;
		draw_list.reserve(entries.size());
		for (const auto& entry : entries)
			draw_list.push_back(entry.second);
		return draw_list;
	}

	/* 阴影投射列表：只挑会写深度图的物体，按剔除方式 + 网格分组 —— 阴影层里投射材质是
	 * 共用的，切换成本全在剔除方式和顶点缓冲上，同组连着画就摊到一次。
	 * 没有材质的物体照样投影，剔除方式沿用投射材质的默认值（base_state）。 */
	inline std::vector<const VisibleMeshObject*> CollectShadowCasterList(
		const std::vector<VisibleMeshObject>& objects, const RenderRasterState& base_state)
	{
		std::vector<std::pair<uint64_t, const VisibleMeshObject*>> entries;
		entries.reserve(objects.size());

		for (const VisibleMeshObject& object : objects)
		{
			if (object.MeshSegment == nullptr)
				continue;

			/* 天空盒铺满全屏（z 落远平面）、透明物体投出来是实心影子、关了投影的不写图 */
			if (object.Material != nullptr && (object.Material->IsSkyBox() || object.Material->IsTransparent()))
				continue;
			if (!object.CastShadow)
				continue;

			const RenderRasterState& object_state =
				(object.Material != nullptr) ? object.Material->GetRasterState() : base_state;

			uint64_t key = 0xCBF29CE484222325ULL;
			const auto mix = [&key](uint64_t value)
			{
				key ^= value;
				key *= 0x100000001B3ULL;
			};
			mix(static_cast<uint64_t>(object_state.CullMode));
			mix(static_cast<uint64_t>(object_state.FrontFaceType));
			mix(reinterpret_cast<uintptr_t>(object.MeshSegment->GetMeshPrimitive().VertexArray.get()));

			entries.emplace_back(key, &object);
		}

		std::stable_sort(entries.begin(), entries.end(),
			[](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

		std::vector<const VisibleMeshObject*> draw_list;
		draw_list.reserve(entries.size());
		for (const auto& entry : entries)
			draw_list.push_back(entry.second);
		return draw_list;
	}
}
