#include "Pch.h"
#include "RenderQuery.h"

namespace Helios
{
	void RenderQueryContext::Init()
	{
		PROFILE_FUNCTION();
		m_QueryNodes.reserve(DEFAULT_QUERY_COUNT);
		for (uint8_t index = 0; index < DEFAULT_QUERY_COUNT; ++index)
			m_QueryNodes.emplace_back(DeviceQueryNode::Create());

		m_pQueryBuffer = DeviceQueryBuffer::Create();
	}

	void RenderQueryContext::Destroy()
	{
		PROFILE_FUNCTION();

		for (auto& query : m_QueryNodes)
			delete query;
		
		m_QueryNodes.clear();

		delete m_pQueryBuffer;
		m_pQueryBuffer = nullptr;
	}

	void RenderQueryContext::BeginFrame(uint32_t frame_id)
	{
		PROFILE_FUNCTION();

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

	void RenderQueryContext::EndFrame()
	{
		PROFILE_FUNCTION();

		while (m_CurrentNodeIndex ^ INVALID_QUERY_NODE_INDEX)
			EndGPUScope();
	}

	void RenderQueryContext::BeginGPUScope(const char* label)
	{
		PROFILE_FUNCTION();

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

	void RenderQueryContext::EndGPUScope()
	{
		PROFILE_FUNCTION();

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
	void RenderQueryContext::OnRenderPassBegin(void* native_render_pass_descriptor, bool clear_only)
	{
		PROFILE_FUNCTION();

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

	void RenderQueryContext::OnRenderPassEnd()
	{
		PROFILE_FUNCTION();

		if (m_pQueryBuffer != nullptr)
			m_pQueryBuffer->OnRenderPassEnd();
	}

	bool RenderQueryContext::PrepareQueryResult()
	{
		PROFILE_FUNCTION();

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

	RenderQueryProfiler::~RenderQueryProfiler()
	{
		Release();
	}

	RenderQueryProfiler& RenderQueryProfiler::Instance()
	{
		static RenderQueryProfiler instance;
		return instance;
	}

	void RenderQueryProfiler::Release()
	{
		for (auto& context : m_QueryContexts)
			context.Destroy();
	}

	void RenderQueryProfiler::BeginFrame(uint32_t frame_id)
	{
		if (!m_IsEnabled)
			return;

		PROFILE_FUNCTION();

		auto& currentContext = m_QueryContexts[m_WriteContextId];
		currentContext.BeginFrame(frame_id);
	}

	void RenderQueryProfiler::EndFrame()
	{
		if (!m_IsEnabled)
			return;

		PROFILE_FUNCTION();

		auto& currentContext = m_QueryContexts[m_WriteContextId];
		currentContext.EndFrame();
	}

	void RenderQueryProfiler::BeginGPUScope(const char* label)
	{
		if (!m_IsEnabled)
			return;

		PROFILE_FUNCTION();

		auto& currentContext = m_QueryContexts[m_WriteContextId];
		currentContext.BeginGPUScope(label);
	}

	void RenderQueryProfiler::EndGPUScope()
	{
		if (!m_IsEnabled)
			return;

		PROFILE_FUNCTION();

		auto& currentContext = m_QueryContexts[m_WriteContextId];
		currentContext.EndGPUScope();
	}

	/* 渲染通道即将开始：转发给写 Context（计入本帧）。计时停用时也调用 ——
	 * 后端实现负责清掉描述符上的残留附件。 */
	void RenderQueryProfiler::OnRenderPassBegin(void* native_render_pass_descriptor)
	{
		auto& writeContext = m_QueryContexts[m_WriteContextId];
		writeContext.OnRenderPassBegin(native_render_pass_descriptor, /*clear_only=*/!m_IsEnabled);
	}

	/* 渲染通道结束（编码器已关闭）：清掉写 Context 的"打开通道"标记 */
	void RenderQueryProfiler::OnRenderPassEnd()
	{
		m_QueryContexts[m_WriteContextId].OnRenderPassEnd();
	}

	bool RenderQueryProfiler::PrepareQueryResult(ResultGPUTimerNode& root_node)
	{
		PROFILE_FUNCTION();

		if (!m_IsEnabled)
		{
			/* 关闭时清空结果，避免UI显示旧数据 */
			root_node = ResultGPUTimerNode{};
			return false;
		}

		auto& readContext = m_QueryContexts[m_ReadContextId];

		if (readContext.PrepareQueryResult())
		{
			std::function<void(ResultGPUTimerNode&, DeviceQueryNode*, ResultGPUTimerNode*)> FillResultGPUTimeRecursively;
			FillResultGPUTimeRecursively = [&FillResultGPUTimeRecursively, &readContext](ResultGPUTimerNode& timerNode, DeviceQueryNode* queryNode, ResultGPUTimerNode* recordNode)
				{
					timerNode.Label = queryNode->Label;
					timerNode.QueryIndex = queryNode->NodeIndex;

					/* 无样本的节点保留上次的平滑值、只标记"无数据"（UI 以占位显示）：
					 * 归零会让它在恢复采样后要从零重新爬升 */
					timerNode.HasData = queryNode->HasValidSamples;
					timerNode.HasStageSplit = queryNode->SupportsStageSplit();
					if (queryNode->HasValidSamples)
					{
						timerNode.GPUTime = timerNode.GPUTime * 0.9 + (queryNode->ResultTimeEnd - queryNode->ResultTimeBegin) *0.1;
						timerNode.GPUTimeVertex = timerNode.GPUTimeVertex * 0.9 + queryNode->ResultVertexUs * 0.1;
						timerNode.GPUTimeFragment = timerNode.GPUTimeFragment * 0.9 + queryNode->ResultFragmentUs * 0.1;
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
							recordNode->GPUTime = queryNode->ResultTimeEnd - queryNode->ResultTimeBegin;
							recordNode->GPUTimeVertex = queryNode->ResultVertexUs;
							recordNode->GPUTimeFragment = queryNode->ResultFragmentUs;
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
						FillResultGPUTimeRecursively(childTimerNode, childQueryNode,
							recordNode != nullptr ? &recordNode->Children[i] : nullptr);
					}
				};

			auto* rootQueryNode = readContext.m_QueryNodes[readContext.m_RootNodeIndex];
			/* 录制：与显示同一次遍历，另填一棵逐帧原始值树（未在录制时为空指针） */
			ResultGPUTimerNode* record_root = m_Recorder.BeginFrame(readContext.m_FrameIndex);
			FillResultGPUTimeRecursively(root_node, rootQueryNode, record_root);

			/* 当一帧数据准备完成时，再切换Context，否则下一帧继续尝试，避免出现刚读完就被刷进新的Query */
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

	RenderQueryProfiler::RenderQueryProfiler()
	{
		PROFILE_FUNCTION();

		/* 初始化RenderQueryContext */
		m_QueryContexts.resize(MAX_CONTEXTS);
		for (auto& context : m_QueryContexts)
			context.Init();
	}
}
