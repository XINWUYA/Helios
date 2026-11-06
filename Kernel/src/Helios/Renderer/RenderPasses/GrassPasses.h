#pragma once
#include <cstdint>

#include "Helios/Common/Export.h"

namespace Helios
{
	class DeviceStorageBuffer;
	class RenderView;

	/* 草地提交：草不是 MeshSegment，不进可见对象列表，由各 Pass 在常规对象之后补一趟 */
	namespace GrassDraw
	{
		/* GPU 剔除：注册一个计算 Pass，按相机与各档级联的视锥把可见实例压实，
		 * 顺带写好间接参数。要排在所有渲染 Pass 之前注册 —— 计算派发会把
		 * 当前渲染编码器收掉，夹在渲染通道中间会把它们切碎 */
		HELIOS_API void AddIndirectCullPass(RenderView& view);

		/* GPU 放置：一次派发算出整片草地的叶片实例，位置、朝向与贴地高度都在着色器里定，
		 * CPU 只负责块划分。高度图与密度遮罩没备好时绑哑缓冲，避免 Metal 缺槽断言。 */
		HELIOS_API bool DispatchPlacement(const SharedPtr<DeviceStorageBuffer>& instances,
			const void* param_data, uint32_t param_size, uint32_t instance_count,
			const SharedPtr<DeviceStorageBuffer>& terrain_heights,
			const SharedPtr<DeviceStorageBuffer>& chunk_indices,
			const SharedPtr<DeviceStorageBuffer>& density_mask);

		/* 间接绘制分四步跑，前两步只筛块，第三步才逐株过，第四步是没有块表时的退化路径。 */
		/* 第一步 重置参数块：每段写成「24 个索引、0 个实例」，同时把可见块计数清零。
		 * 24 是叶片模板的索引数；实例数由后面两步累加，不归零会把上一帧的残留画进去。 */
		HELIOS_API bool DispatchArgsReset(const SharedPtr<DeviceStorageBuffer>& args,
			const SharedPtr<DeviceStorageBuffer>& chunk_list,
			uint32_t index_count, uint32_t segments, uint32_t args_stride);
		/* 第二步 块级剔除：用块的包围球逐个测光源视锥，通过的块号追加进可见块表，
		 * 最后一个完成的线程把可见块数写成下一阶段的间接派发参数。
		 * 目的是让逐株剔除只扫这几块，近处级联不用陪跑整片草地。 */
		HELIOS_API bool DispatchChunkCull(const SharedPtr<DeviceStorageBuffer>& chunk_bounds,
			const SharedPtr<DeviceStorageBuffer>& chunk_list, const SharedPtr<DeviceStorageBuffer>& args,
			const void* param_data, uint32_t param_size, uint32_t thread_count);
		/* 第三步 逐株剔除：只遍历可见块表里的块，每株再测抽稀曲线与密度，
		 * 通过的实例压实进目标缓冲，段号与实例数原子累加写回参数块。
		 * 按可见块数间接派发；后端没有间接派发就退回按总株数上界派发。 */
		HELIOS_API bool DispatchCull(const SharedPtr<DeviceStorageBuffer>& source,
			const SharedPtr<DeviceStorageBuffer>& target, const SharedPtr<DeviceStorageBuffer>& args,
			const SharedPtr<DeviceStorageBuffer>& chunk_list,
			const void* param_data, uint32_t param_size, uint32_t fallback_instance_count,
			const SharedPtr<DeviceStorageBuffer>& dispatch_args, uint32_t dispatch_offset);
		/* 无块表时的退化路径：每个实例自己测视锥，开销与总株数成正比，只在块掩码关闭时走。
		 * 与第三步写同一份参数块，绘制端不用区分。 */
		HELIOS_API bool DispatchCullFull(const SharedPtr<DeviceStorageBuffer>& source,
			const SharedPtr<DeviceStorageBuffer>& target, const SharedPtr<DeviceStorageBuffer>& args,
			const SharedPtr<DeviceStorageBuffer>& chunk_bounds,
			const void* param_data, uint32_t param_size, uint32_t instance_count);
	}
}
