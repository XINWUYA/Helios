#pragma once

namespace Helios
{
	constexpr uint32_t INVALID_QUERY_NODE_INDEX = UINT32_MAX;
	constexpr uint64_t INVALID_QUERY_TIME = UINT64_MAX;

	struct GPUPassTimestamp;

	/* RenderQueryNode基类
	 * 用于记录一个GPU区间的一对时间戳
	 */
	struct DeviceQueryNode
	{
		virtual ~DeviceQueryNode() = default;

		virtual void Begin() {}
		virtual void End() {}
		virtual bool GetQueryResult() { return true; }

		/* 渲染通道开始的通知（后端创建编码器之前发出）：只支持"通道采样"的后端（Metal）用它把
		 * 作用域绑到通道序号上（耗时 = 绑定通道的 stage 时长之和）；GL 跟通道无关、保持空实现。 */
		virtual void OnRenderPassBegin(uint32_t /*pass_slot*/) {}

		/* 通道时间戳表注入（解析前由 Context 调用，table 可空 = 本帧无数据）：
		 * 通道采样模型下的节点从这里取值；仅此类后端实现。 */
		virtual void SetPassTimestamps(const std::vector<GPUPassTimestamp>* /*pass_timestamps*/) {}

		std::string Label{ "Unnamed QueryNode" };
		uint32_t NodeIndex{ 0 };
		uint32_t ParentNodeIndex{ INVALID_QUERY_NODE_INDEX };
		std::vector<uint32_t> ChildrenNodeIndices{};
		uint64_t QueryTimeBegin = INVALID_QUERY_TIME;
		uint64_t QueryTimeEnd = INVALID_QUERY_TIME;

		/* 结果区间：下游统一按 ResultTimeEnd - ResultTimeBegin 取值。
		 * 通道采样模型（Metal）下 begin 恒 0、end = 绑定的通道时长和。 */
		double ResultTimeBegin = 0.0;
		double ResultTimeEnd = 0.0;

		/* 顶点 / 片元阶段的细分和（通道采样模型；其余后端保持 0）—— 诊断用 */
		double ResultVertexUs{ 0.0 };
		double ResultFragmentUs{ 0.0 };

		/* 本帧是否取到有效样本：无样本（后端不支持 / 作用域内没有可采样的通道）
		 * 的节点在结果树里标为"无数据"由 UI 占位显示，不拖垮其它节点。 */
		bool HasValidSamples{ false };

		static DeviceQueryNode* Create();
	};
}