#include "Pch.h"
#include "GPUTimeScopeTree.h"
#include <Helios/Renderer/Renderer.h>

namespace Helios
{
	void GPUQueryContext::Init()
	{
		PROFILE_CPU_FUNCTION();
		m_QueryNodes.reserve(DEFAULT_QUERY_COUNT);
		for (uint8_t index = 0; index < DEFAULT_QUERY_COUNT; ++index)
			m_QueryNodes.emplace_back(DeviceQueryNode::Create());

		m_pQueryBuffer = DeviceQueryBuffer::Create();
	}

	void GPUQueryContext::Destroy()
	{
		PROFILE_CPU_FUNCTION();

		for (auto& query : m_QueryNodes)
			delete query;
		
		m_QueryNodes.clear();

		delete m_pQueryBuffer;
		m_pQueryBuffer = nullptr;
	}

	void GPUQueryContext::BeginFrame(uint32_t frame_id)
	{
		PROFILE_CPU_FUNCTION();

		m_IsValid = true;
		m_RootNodeIndex = INVALID_QUERY_NODE_INDEX;
		m_CurrentNodeIndex = INVALID_QUERY_NODE_INDEX;
		m_UsedNodeIndex = 0;
		m_FrameIndex = frame_id;
		m_OpenNodeIndices.clear();
		m_PassTimestamps.clear();
		if (m_pQueryBuffer != nullptr)
			m_pQueryBuffer->ResetFrame();

		for (auto& queryNode : m_QueryNodes)
		{
			queryNode->Label = "";
			queryNode->NodeIndex = 0;
			queryNode->ParentNodeIndex = INVALID_QUERY_NODE_INDEX;
			queryNode->ChildrenNodeIndices.clear();
			queryNode->QueryTimeBegin = INVALID_QUERY_TIME;
			queryNode->QueryTimeEnd = INVALID_QUERY_TIME;
			queryNode->ResultTimeBegin = 0.0;
			queryNode->ResultTimeEnd = 0.0;
			queryNode->HasValidSamples = false;
		}

		BeginGPUScope("FrameTotal");
	}

	void GPUQueryContext::EndFrame()
	{
		PROFILE_CPU_FUNCTION();

		while (m_CurrentNodeIndex ^ INVALID_QUERY_NODE_INDEX)
			EndGPUScope();
	}

	void GPUQueryContext::BeginGPUScope(const char* label)
	{
		PROFILE_CPU_FUNCTION();

		if (m_UsedNodeIndex >= m_QueryNodes.size())
		{
			m_QueryNodes.emplace_back(DeviceQueryNode::Create());
		}

		auto& newQueryNode = m_QueryNodes[m_UsedNodeIndex];
		newQueryNode->NodeIndex = m_UsedNodeIndex++; /* 递增m_UsedNodeIndex */
		newQueryNode->Label = label;

		if (m_CurrentNodeIndex ^ INVALID_QUERY_NODE_INDEX)
		{
			newQueryNode->ParentNodeIndex = m_CurrentNodeIndex;
			auto* currentQueryNode = m_QueryNodes[m_CurrentNodeIndex];
			currentQueryNode->ChildrenNodeIndices.emplace_back(newQueryNode->NodeIndex);
			m_CurrentNodeIndex = newQueryNode->NodeIndex;
		}
		else
		{
			m_RootNodeIndex = m_CurrentNodeIndex = newQueryNode->NodeIndex;
		}

		m_OpenNodeIndices.emplace_back(newQueryNode->NodeIndex);

		/* 开始当前Query */
		newQueryNode->Begin();

		/* 作用域在通道内开启（作用域边界晚于编码器创建 —— 如默认目标 Pass 的
		 * BeginDefaultRenderPass 先于 BeforeExecute）：补绑当前打开的通道，
		 * 否则该作用域永远等不到"通道开始"事件、只能显示无数据。 */
		if (m_pQueryBuffer != nullptr)
		{
			const uint32_t open_pass_slot = m_pQueryBuffer->GetOpenRenderPassSlot();
			if (open_pass_slot != DeviceQueryBuffer::INVALID_PASS_SLOT)
				newQueryNode->OnRenderPassBegin(open_pass_slot);
		}
	}

	void GPUQueryContext::EndGPUScope()
	{
		PROFILE_CPU_FUNCTION();

		if (m_CurrentNodeIndex ^ INVALID_QUERY_NODE_INDEX)
		{
			auto* currentQueryNode = m_QueryNodes[m_CurrentNodeIndex];
			currentQueryNode->End();
			if (!m_OpenNodeIndices.empty() && m_OpenNodeIndices.back() == m_CurrentNodeIndex)
				m_OpenNodeIndices.pop_back();
			m_CurrentNodeIndex = currentQueryNode->ParentNodeIndex;
		}
	}

	/* 渲染通道开始：给通道分配一对采样下标并挂到描述符上，再把它通知给各打开的作用域。
	 * clear_only（计时停用 / 未使用）时只清残留 —— 描述符跨帧复用，不清的话本通道会
	 * 继续往上一帧的缓冲里写。 */
	void GPUQueryContext::OnRenderPassBegin(void* native_render_pass_descriptor, bool clear_only)
	{
		PROFILE_CPU_FUNCTION();

		if (m_pQueryBuffer == nullptr || native_render_pass_descriptor == nullptr)
			return;

		if (clear_only || !m_IsValid)
		{
			m_pQueryBuffer->ClearRenderPassAttachment(native_render_pass_descriptor);
			return;
		}

		const uint32_t pass_slot = m_pQueryBuffer->AttachRenderPass(native_render_pass_descriptor);
		if (pass_slot == DeviceQueryBuffer::INVALID_PASS_SLOT)
			return;

		for (const uint32_t node_index : m_OpenNodeIndices)
			m_QueryNodes[node_index]->OnRenderPassBegin(pass_slot);
	}

	void GPUQueryContext::OnRenderPassEnd()
	{
		PROFILE_CPU_FUNCTION();

		if (m_pQueryBuffer != nullptr)
			m_pQueryBuffer->OnRenderPassEnd();
	}

	/* 计算通道开始：分配采样槽位并挂到描述符上，再把它通知给各打开的作用域。
	 * 描述符每次派发新建，没有跨帧残留可清 —— 停用 / 不可用时直接不采样。 */
	void GPUQueryContext::OnComputePassBegin(void* native_compute_pass_descriptor, bool clear_only)
	{
		PROFILE_CPU_FUNCTION();

		if (m_pQueryBuffer == nullptr || native_compute_pass_descriptor == nullptr)
			return;

		if (clear_only || !m_IsValid)
			return;

		const uint32_t pass_slot = m_pQueryBuffer->AttachComputePass(native_compute_pass_descriptor);
		if (pass_slot == DeviceQueryBuffer::INVALID_PASS_SLOT)
			return;

		for (const uint32_t node_index : m_OpenNodeIndices)
			m_QueryNodes[node_index]->OnRenderPassBegin(pass_slot);
	}

	bool GPUQueryContext::PrepareQueryResult()
	{
		PROFILE_CPU_FUNCTION();

		if (m_RootNodeIndex == INVALID_QUERY_NODE_INDEX)
			return false;

		/* 通道采样模型：先把通道时间戳表解析出来再问各节点（所有节点共用一份数据） */
		if (m_pQueryBuffer != nullptr)
		{
			if (!m_pQueryBuffer->TryResolvePassTimestamps(m_PassTimestamps))
				return false; /* 结果尚不可用：稍后重试 */

			const std::vector<GPUPassTimestamp>* pass_timestamps = m_PassTimestamps.empty() ? nullptr : &m_PassTimestamps;
			for (uint32_t i = 0; i < m_UsedNodeIndex; ++i)
				m_QueryNodes[i]->SetPassTimestamps(pass_timestamps);
		}

		for (uint32_t i = 0; i < m_UsedNodeIndex; ++i)
		{
			/* 确保所有节点都获取成功 */
			if (!m_QueryNodes[i]->GetQueryResult())
				return false;
		}

		return true;
	}

	GPUTimeScopeTree::~GPUTimeScopeTree()
	{
		/* 后端不持有时序保证，退出前先把监听摘掉 */
		if (const SharedPtr<RenderAPI>& api = Renderer::GetRenderAPI())
			api->SetGPUScopeInterface(nullptr);

		Release();
	}

	void GPUTimeScopeTree::SetEnabled(bool enabled)
	{
		m_IsEnabled = enabled;

		/* 立即同步给设备层：关掉后后端不再上报通道事件、也不再为计算通道建描述符 */
		if (const SharedPtr<RenderAPI>& api = Renderer::GetRenderAPI())
		{
			api->SetGPUScopeInterface(enabled ? static_cast<GPUScopeInterface*>(this) : nullptr);
			api->SetGpuTimingActive(enabled);
		}
	}

	GPUTimeScopeTree& GPUTimeScopeTree::Instance()
	{
		static GPUTimeScopeTree instance;
		return instance;
	}

	void GPUTimeScopeTree::Release()
	{
		for (auto& context : m_QueryContexts)
			context.Destroy();
	}

	void GPUTimeScopeTree::BeginFrame(uint32_t frame_id)
	{
		/* 每帧对齐设备层状态：设备可能在本帧重建，监听指针要重新挂上 */
		if (const SharedPtr<RenderAPI>& api = Renderer::GetRenderAPI())
		{
			api->SetGPUScopeInterface(m_IsEnabled ? static_cast<GPUScopeInterface*>(this) : nullptr);
			api->SetGpuTimingActive(m_IsEnabled);
		}

		if (!m_IsEnabled)
			return;

		PROFILE_CPU_FUNCTION();

		auto& currentContext = m_QueryContexts[m_WriteContextId];
		currentContext.BeginFrame(frame_id);
	}

	void GPUTimeScopeTree::EndFrame()
	{
		if (!m_IsEnabled)
			return;

		PROFILE_CPU_FUNCTION();

		auto& currentContext = m_QueryContexts[m_WriteContextId];
		currentContext.EndFrame();
	}

	void GPUTimeScopeTree::BeginGPUScope(const char* label)
	{
		if (!m_IsEnabled)
			return;

		PROFILE_CPU_FUNCTION();

		auto& currentContext = m_QueryContexts[m_WriteContextId];
		currentContext.BeginGPUScope(label);
	}

	void GPUTimeScopeTree::EndGPUScope()
	{
		if (!m_IsEnabled)
			return;

		PROFILE_CPU_FUNCTION();

		auto& currentContext = m_QueryContexts[m_WriteContextId];
		currentContext.EndGPUScope();
	}

	/* 渲染通道即将开始：转发给写 Context（计入本帧）。计时停用时也调用 ——
	 * 后端实现负责清掉描述符上的残留附件。 */
	void GPUTimeScopeTree::OnRenderPassBegin(void* native_render_pass_descriptor)
	{
		auto& writeContext = m_QueryContexts[m_WriteContextId];
		writeContext.OnRenderPassBegin(native_render_pass_descriptor, /*clear_only=*/!m_IsEnabled);
	}

	/* 渲染通道结束（编码器已关闭）：清掉写 Context 的"打开通道"标记 */
	void GPUTimeScopeTree::OnRenderPassEnd()
	{
		m_QueryContexts[m_WriteContextId].OnRenderPassEnd();
	}

	void GPUTimeScopeTree::OnComputePassBegin(void* native_compute_pass_descriptor)
	{
		m_QueryContexts[m_WriteContextId].OnComputePassBegin(native_compute_pass_descriptor,
			/*clear_only=*/!m_IsEnabled);
	}

	bool GPUTimeScopeTree::PrepareQueryResult()
	{
		PROFILE_CPU_FUNCTION();

		if (!m_IsEnabled)
		{
			/* 关闭时清空结果，避免UI显示旧数据 */
			m_RealtimeRoot = GPUTimeScopeNode{};
			return false;
		}

		auto& readContext = m_QueryContexts[m_ReadContextId];

		if (readContext.PrepareQueryResult())
		{
			std::function<void(GPUTimeScopeNode&, DeviceQueryNode*, GPUTimeScopeNode*)> FillResultTotalUsRecursively;
			FillResultTotalUsRecursively = [&FillResultTotalUsRecursively, &readContext](GPUTimeScopeNode& timerNode, DeviceQueryNode* queryNode, GPUTimeScopeNode* recordNode)
				{
					timerNode.Label = queryNode->Label;
					timerNode.QueryIndex = queryNode->NodeIndex;

					/* 无样本的节点保留上次的平滑值、只标记"无数据"（UI 以占位显示）：
					 * 归零会让它在恢复采样后要从零重新爬升 */
					timerNode.HasData = queryNode->HasValidSamples;
					timerNode.HasStageSplit = queryNode->SupportsStageSplit();
					if (queryNode->HasValidSamples)
					{
						timerNode.TotalUs = timerNode.TotalUs * 0.9 + (queryNode->ResultTimeEnd - queryNode->ResultTimeBegin) *0.1;
						timerNode.StageVertexUs = timerNode.StageVertexUs * 0.9 + queryNode->ResultVertexUs * 0.1;
						timerNode.StageFragmentUs = timerNode.StageFragmentUs * 0.9 + queryNode->ResultFragmentUs * 0.1;
						timerNode.StageComputeUs = timerNode.StageComputeUs * 0.9 + queryNode->ResultComputeUs * 0.1;
					}

					/* 录制树（可选）：写入当帧原始值 —— 逐帧回放要看每帧真值，不做平滑 */
					if (recordNode != nullptr)
					{
						recordNode->Label = queryNode->Label;
						recordNode->QueryIndex = queryNode->NodeIndex;
						recordNode->HasData = queryNode->HasValidSamples;
						recordNode->HasStageSplit = queryNode->SupportsStageSplit();
						if (queryNode->HasValidSamples)
						{
							recordNode->TotalUs = queryNode->ResultTimeEnd - queryNode->ResultTimeBegin;
							recordNode->StageVertexUs = queryNode->ResultVertexUs;
							recordNode->StageFragmentUs = queryNode->ResultFragmentUs;
							recordNode->StageComputeUs = queryNode->ResultComputeUs;
						}
					}

					const size_t childCount = queryNode->ChildrenNodeIndices.size();
					timerNode.Children.resize(childCount);
					if (recordNode != nullptr)
						recordNode->Children.resize(childCount);
					for (size_t i = 0; i < childCount; ++i)
					{
						auto& childTimerNode = timerNode.Children[i];
						auto* childQueryNode = readContext.m_QueryNodes[queryNode->ChildrenNodeIndices[i]];
						FillResultTotalUsRecursively(childTimerNode, childQueryNode,
							recordNode != nullptr ? &recordNode->Children[i] : nullptr);
					}
				};

			auto* rootQueryNode = readContext.m_QueryNodes[readContext.m_RootNodeIndex];
			/* 录制：与显示同一次遍历，另填一棵逐帧原始值树（未在录制时为空指针） */
			GPUTimeScopeNode* record_root = m_Recorder.BeginFrame(readContext.m_FrameIndex);
			FillResultTotalUsRecursively(m_RealtimeRoot, rootQueryNode, record_root);

			/* 当一帧数据准备完成时，再切换Context，否则下一帧继续尝试，避免出现刚读完就被刷进新的Query */
			m_ResultFrameId = readContext.m_FrameIndex;
			m_WriteContextId = (m_WriteContextId + 1) % MAX_CONTEXTS;
			m_ReadContextId = (m_ReadContextId + 1) % MAX_CONTEXTS;
			return true;
		}

		if (!readContext.m_IsValid)
		{
			/* 当Context未被使用时，也进行切换 */
			m_WriteContextId = (m_WriteContextId + 1) % MAX_CONTEXTS;
			m_ReadContextId = (m_ReadContextId + 1) % MAX_CONTEXTS;
		}
		return false;
	}

	GPUTimeScopeTree::GPUTimeScopeTree()
	{
		PROFILE_CPU_FUNCTION();

		/* 初始化GPUQueryContext */
		m_QueryContexts.resize(MAX_CONTEXTS);
		for (auto& context : m_QueryContexts)
			context.Init();
	}
}
