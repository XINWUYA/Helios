#include "Pch.h"
#include "CPUTimerProfiler.h"
#include <cstring>

namespace Helios
{
	CPUTimerProfiler& CPUTimerProfiler::Instance()
	{
		static CPUTimerProfiler instance;
		return instance;
	}

	void CPUTimerProfiler::SetEnabled(bool enabled)
	{
		if (m_Enabled == enabled)
			return;
		m_Enabled = enabled;

		if (!enabled)
		{
			/* 关闭：丢弃进行中的帧与实时树（面板转 Off 占位）；在途 scope 照常析构
			 * 到空栈上，被静默忽略 */
			m_FrameBegun = false;
			m_OpenIndices.clear();
			m_UsedNodeCount = 0;
			m_RealtimeRoot = ResultCPUTimerNode{};
		}
	}

	void CPUTimerProfiler::BeginFrame()
	{
		if (!m_Enabled)
			return;

		m_OwnerThread = std::this_thread::get_id();
		++m_FrameId;
		m_FrameBegun = true;
		m_UsedNodeCount = 0;
		m_OpenIndices.clear();
		m_FrameStart = std::chrono::steady_clock::now();

		/* 帧根：栈空时的第一个作用域（节点 0） */
		BeginScope("FrameTotal");
	}

	void CPUTimerProfiler::EndFrame()
	{
		if (!m_Enabled || !m_FrameBegun)
			return;

		/* 防御：未闭合的作用域（异常路径）全部收掉 —— 帧根也在其中 */
		while (!m_OpenIndices.empty())
			EndScope(m_OpenIndices.back());

		if (m_UsedNodeCount > 0)
		{
			SettleRealtime(m_RealtimeRoot, 0);
			if (ResultCPUTimerNode* record_root = m_Recorder.BeginFrame(m_FrameId))
				SettleRaw(*record_root, 0);
		}

		m_FrameBegun = false;
	}

	uint32_t CPUTimerProfiler::BeginScope(const char* label)
	{
		/* 帧外（启动 / 静态初始化）与非 owner 线程（worker）静默跳过，避免池竞争 */
		if (!m_FrameBegun || std::this_thread::get_id() != m_OwnerThread)
			return INVALID_CPU_SCOPE_INDEX;

		const uint32_t parent_index = m_OpenIndices.empty()
			? INVALID_CPU_SCOPE_INDEX : m_OpenIndices.back();

		uint32_t index = INVALID_CPU_SCOPE_INDEX;
		if (parent_index == INVALID_CPU_SCOPE_INDEX)
			index = AllocNode(label, INVALID_CPU_SCOPE_INDEX);  /* 帧根 */
		else
		{
			index = FindChild(parent_index, label);
			if (index == INVALID_CPU_SCOPE_INDEX)
				index = AllocNode(label, parent_index);
		}

		ScopeNode& node = m_ScopeNodes[index];
		if (node.ActiveCount == 0)
		{
			node.EntryUs = ElapsedUs();
			if (node.CallCount == 0 || node.EntryUs < node.BeginUs)
				node.BeginUs = node.EntryUs;
		}
		++node.ActiveCount;
		++node.CallCount;

		m_OpenIndices.emplace_back(index);
		return index;
	}

	void CPUTimerProfiler::EndScope(uint32_t node_index)
	{
		/* 栈顶校验：帧被重置（关闭 / 跨帧乱序析构）后旧 scope 静默忽略 */
		if (m_OpenIndices.empty() || m_OpenIndices.back() != node_index)
			return;
		m_OpenIndices.pop_back();

		ScopeNode& node = m_ScopeNodes[node_index];
		if (node.ActiveCount > 0)
			--node.ActiveCount;

		const double now_us = ElapsedUs();
		if (now_us > node.EndUs)
			node.EndUs = now_us;
		if (node.ActiveCount == 0)
			node.AccumulatedUs += now_us - node.EntryUs;
	}

	uint32_t CPUTimerProfiler::AllocNode(const char* label, uint32_t parent_index)
	{
		if (m_UsedNodeCount >= m_ScopeNodes.size())
			m_ScopeNodes.emplace_back();

		const uint32_t index = m_UsedNodeCount++;
		ScopeNode& node = m_ScopeNodes[index];
		node.Label = label;
		node.ParentIndex = parent_index;
		node.ChildrenIndices.clear();
		node.AccumulatedUs = 0.0;
		node.BeginUs = node.EndUs = node.EntryUs = 0.0;
		node.ActiveCount = 0;
		node.CallCount = 0;

		if (parent_index != INVALID_CPU_SCOPE_INDEX)
			m_ScopeNodes[parent_index].ChildrenIndices.emplace_back(index);
		return index;
	}

	uint32_t CPUTimerProfiler::FindChild(uint32_t parent_index, const char* label) const
	{
		const ScopeNode& parent = m_ScopeNodes[parent_index];
		for (const uint32_t child_index : parent.ChildrenIndices)
		{
			const char* child_label = m_ScopeNodes[child_index].Label;
			if (child_label == label || std::strcmp(child_label, label) == 0)
				return child_index;
		}
		return INVALID_CPU_SCOPE_INDEX;
	}

	double CPUTimerProfiler::ElapsedUs() const
	{
		return std::chrono::duration<double, std::micro>(
			std::chrono::steady_clock::now() - m_FrameStart).count();
	}

	/* 递归结算（先父后子）：EMA 只对有效样本累积；节点标签换位（结构变化）时
	 * 整棵子树作废重来，避免把不同函数的值混在一起 */
	void CPUTimerProfiler::SettleRealtime(ResultCPUTimerNode& out, uint32_t index) const
	{
		const ScopeNode& raw = m_ScopeNodes[index];

		if (out.Label != raw.Label)
		{
			out.Label = raw.Label;
			out.TotalUs = 0.0;
			out.SelfUs = 0.0;
			out.HasData = false;
		}

		double children_total = 0.0;
		for (const uint32_t child_index : raw.ChildrenIndices)
			children_total += m_ScopeNodes[child_index].AccumulatedUs;
		double self = raw.AccumulatedUs - children_total;
		if (self < 0.0)
			self = 0.0;

		if (!out.HasData)
		{
			out.TotalUs = raw.AccumulatedUs;  /* 首次直接落值 */
			out.SelfUs = self;
			out.HasData = true;
		}
		else
		{
			out.TotalUs = out.TotalUs * 0.9 + raw.AccumulatedUs * 0.1;
			out.SelfUs = out.SelfUs * 0.9 + self * 0.1;
		}
		out.BeginUs = raw.BeginUs;
		out.EndUs = raw.EndUs;
		out.CallCount = raw.CallCount;

		out.Children.resize(raw.ChildrenIndices.size());
		for (size_t i = 0; i < raw.ChildrenIndices.size(); ++i)
			SettleRealtime(out.Children[i], raw.ChildrenIndices[i]);
	}

	/* 原始值结算（录制版；逐帧回放要真值，不做任何平滑） */
	void CPUTimerProfiler::SettleRaw(ResultCPUTimerNode& out, uint32_t index) const
	{
		const ScopeNode& raw = m_ScopeNodes[index];

		double children_total = 0.0;
		for (const uint32_t child_index : raw.ChildrenIndices)
			children_total += m_ScopeNodes[child_index].AccumulatedUs;
		double self = raw.AccumulatedUs - children_total;
		if (self < 0.0)
			self = 0.0;

		out.Label = raw.Label;
		out.TotalUs = raw.AccumulatedUs;
		out.SelfUs = self;
		out.BeginUs = raw.BeginUs;
		out.EndUs = raw.EndUs;
		out.CallCount = raw.CallCount;
		out.HasData = true;

		out.Children.resize(raw.ChildrenIndices.size());
		for (size_t i = 0; i < raw.ChildrenIndices.size(); ++i)
			SettleRaw(out.Children[i], raw.ChildrenIndices[i]);
	}

	ScopedCPUTimerScope::ScopedCPUTimerScope(const char* label)
	{
		CPUTimerProfiler& profiler = CPUTimerProfiler::Instance();
		if (!profiler.IsEnabled())
			return;

		m_NodeIndex = profiler.BeginScope(label);
	}

	ScopedCPUTimerScope::~ScopedCPUTimerScope()
	{
		if (m_NodeIndex == INVALID_CPU_SCOPE_INDEX)
			return;
		CPUTimerProfiler::Instance().EndScope(m_NodeIndex);
	}
}
