#pragma once
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace Helios
{
	/* 一个通道的采样结果：各阶段在帧内的绝对起止，单位 us，帧首为 0，没采到样就是 0。
	 * 作用域耗时由这些窗口求并集，不另存时长结论。片元起点已做排队修正，可直接用。 */
	struct GPUPassTimestamp
	{
		double VertexBeginUs{ 0.0 };
		double VertexEndUs{ 0.0 };
		double FragmentBeginUs{ 0.0 };
		double FragmentEndUs{ 0.0 };
		double ComputeBeginUs{ 0.0 };
		double ComputeEndUs{ 0.0 };

		bool HasVertex() const { return VertexEndUs > VertexBeginUs; }
		bool HasFragment() const { return FragmentEndUs > FragmentBeginUs; }
		bool HasCompute() const { return ComputeEndUs > ComputeBeginUs; }
	};

	/* 求一组时间窗口的并集，重叠的段只算一次。
	 * 相邻通道的顶点与片元会交叠，逐个相加会把同一段时间重复计。允许乱序，内部排序。 */
	inline double UnionSpanUs(std::vector<std::pair<double, double>> spans)
	{
		if (spans.empty())
			return 0.0;

		std::sort(spans.begin(), spans.end());

		double union_us = 0.0;
		double run_begin = spans.front().first;
		double run_end = spans.front().second;
		for (size_t index = 1; index < spans.size(); ++index)
		{
			const auto& [begin, end] = spans[index];
			if (begin <= run_end)
			{
				if (end > run_end)
					run_end = end;
				continue;
			}

			union_us += run_end - run_begin;
			run_begin = begin;
			run_end = end;
		}
		return union_us + (run_end - run_begin);
	}

	/* 一帧 GPU 时间戳的后端存放处（通道采样模型）：每个通道占四个采样下标
	 * （顶点与片元各起止，计算通道只用前两个），开始时挂描述符、结束后解析成表。
	 * Apple GPU 只在通道边界采样，耗时要靠区间并集合成；GL 用时间戳查询，Create 返回空。 */
	class DeviceQueryBuffer
	{
	public:
		virtual ~DeviceQueryBuffer() = default;

		/* 每个渲染通道占用的采样下标数（顶点起/止 + 片元起/止） */
		static constexpr uint32_t SAMPLES_PER_PASS = 4;
		/* 一帧的渲染通道数上限（采样缓冲按此预分配；超出部分的通道不采样） */
		static constexpr uint32_t MAX_PASS_COUNT = 128;
		/* 未分配 / 不可用的通道序号 */
		static constexpr uint32_t INVALID_PASS_SLOT = UINT32_MAX;

		static DeviceQueryBuffer* Create();

		/* 渲染通道开始（创建编码器前调用）：把本通道的一对采样下标挂到通道描述符上，
		 * 返回本通道的采样序号；不采样 / 分配失败返回 INVALID_PASS_SLOT，
		 * 且必须保证描述符上没有残留的旧附件（描述符跨帧复用）。 */
		virtual uint32_t AttachRenderPass(void* native_render_pass_descriptor) = 0;

		/* 清除通道描述符上可能残留的采样附件（计时停用 / 通道不采样时调用） */
		virtual void ClearRenderPassAttachment(void* native_render_pass_descriptor) = 0;

		/* 计算通道开始（创建编码器前调用）：与渲染通道共用槽位空间，只占首尾两个采样点，
		 * 返回采样槽位序号；不可用时返回 INVALID_PASS_SLOT（描述符照常能拿来建编码器）。
		 * 不采样的后端（GL）保持默认实现。 */
		virtual uint32_t AttachComputePass(void* /*native_compute_pass_descriptor*/)
		{
			return INVALID_PASS_SLOT;
		}

		/* 当前仍打开的采样通道序号：作用域可能在通道内才开启（如默认目标 Pass ——
		 * 编码器先于作用域创建），Context 靠它把新节点补绑到当前通道；
		 * 没有打开的通道返回 INVALID_PASS_SLOT。 */
		virtual uint32_t GetOpenRenderPassSlot() const { return INVALID_PASS_SLOT; }

		/* 渲染通道结束（编码器已关闭）：清掉"打开"标记 */
		virtual void OnRenderPassEnd() {}

		/* 结果解析：命令缓冲区执行完成时返回 true，并把各通道的时间戳写入 timestamps；
		 * 结果尚未就绪返回 false（调用方稍后重试）；空帧（无通道）返回 true + 空表。 */
		virtual bool TryResolvePassTimestamps(std::vector<GPUPassTimestamp>& timestamps) = 0;

		/* 帧边界：释放上一帧的命令缓冲区引用、重置通道计数 */
		virtual void ResetFrame() {}
	};
}
