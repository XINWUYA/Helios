#pragma once
#include <cstdint>
#include <vector>

namespace Helios
{
	/* 一个渲染通道的 GPU 实际工作时长（通道采样模型；单位 us）：取"各 stage 内的 work 时长"、
	 * 而不是"通道首尾跨度"（TBDR 上阶段会交叠，首尾会把流水线等待算进来）；跟 Xcode 抓帧同口径、
	 * 各作用域可以加。FragmentUs 含排队修正；0 = 没有采样数据。 */
	struct GPUPassTimestamp
	{
		double VertexUs{ 0.0 };
		double FragmentUs{ 0.0 };
		double DurationUs() const { return VertexUs + FragmentUs; }
	};

	/* 一帧 GPU 时间戳的后端存放处（通道采样模型）：每个通道占四个采样下标（顶点起止 + 片元
	 * 起止），通道开始前挂到描述符上、命令完成后一次性解析成表。Apple GPU 只有通道的阶段边界
	 * 能采样 —— 耗时以通道为单位收集再求和；跟通道无关的后端（GL）Create 返回空。 */
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
