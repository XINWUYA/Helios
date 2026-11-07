#include "Pch.h"
#include "CPUTimeScopeTree.h"

namespace Helios
{
	CPUTimeScopeTree& CPUTimeScopeTree::Instance()
	{
		static CPUTimeScopeTree instance;
		return instance;
	}

	void CPUTimeScopeTree::SetEnabled(bool enabled)
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
			m_FramesSinceSettle = 0;
			m_RealtimeRoot = CPUTimeScopeNode{};
		}
	}

	void CPUTimeScopeTree::BeginFrame()
	{
		/* 帧号无条件递增：面板按帧号对齐 CPU / GPU 两条录制，且与 Renderer 的
		 * FrameCounter 逐帧同步 —— 计时关掉再打开也不会错位 */
		++m_FrameId;

		if (!m_Enabled)
			return;

		m_OwnerThread = std::this_thread::get_id();
		m_FrameBegun = true;
		m_UsedNodeCount = 0;
		m_OpenIndices.clear();
		m_FrameStart = std::chrono::steady_clock::now();

		/* 帧根：栈空时的第一个作用域（节点 0）——合成节点，无插桩点位置 */
		BeginScope("FrameTotal", ProfileHash::Hash("FrameTotal"), ProfileChannel::General, nullptr, 0);
	}

	void CPUTimeScopeTree::EndFrame()
	{
		if (!m_Enabled || !m_FrameBegun)
			return;

		/* 防御：未闭合的作用域（异常路径）全部收掉 —— 帧根也在其中 */
		while (!m_OpenIndices.empty())
			EndScope(m_OpenIndices.back());

		if (m_UsedNodeCount > 0)
		{
			/* 等 GPU：先只写录制，那一帧的 GPU 结果到了由 SettleRealtimeFrame 补结算；
			 * 连续超过 kSettleTimeout 帧都没等到（GPU 计时关掉 / 结果卡住）就按当前帧兜底，
			 * 别让实时树停住 —— 计数清零后下一轮还能再兜 */
			++m_FramesSinceSettle;
			if (!m_WaitForGpu || m_FramesSinceSettle > kSettleTimeout)
			{
				SettleRealtime(m_RealtimeRoot, 0);
				m_FramesSinceSettle = 0;
			}

			if (CPUTimeScopeNode* record_root = m_Recorder.BeginFrame(m_FrameId))
				SettleRaw(*record_root, 0);
		}

		m_FrameBegun = false;
	}

	/* 等 GPU 时的补结算：把某一帧的原始树并进实时树（EMA 口径与 SettleRealtime 一致）。
	 * 结构对不上（节点换了位置）时该子树直接落到原始值，不混不同函数的数据。 */
	void CPUTimeScopeTree::SettleRealtimeFrame(uint32_t frame_id)
	{
		if (!m_Enabled)
			return;

		const uint32_t index = m_Recorder.FindIndexByFrameId(frame_id);
		if (index == CPUTimeScopeRecorder::INVALID_FRAME_INDEX)
			return; /* 已经不在环形窗口里：跳过 */

		MergeRealtime(m_RealtimeRoot, m_Recorder.GetFrame(index));
		m_FramesSinceSettle = 0;
	}

	void CPUTimeScopeTree::MergeRealtime(CPUTimeScopeNode& out, const CPUTimeScopeNode& raw)
	{
		const bool same_node = out.HasData && out.Label == raw.Label;
		if (same_node)
		{
			out.TotalUs = out.TotalUs * 0.9 + raw.TotalUs * 0.1;
			out.SelfUs = out.SelfUs * 0.9 + raw.SelfUs * 0.1;
		}
		else
		{
			out.TotalUs = raw.TotalUs;
			out.SelfUs = raw.SelfUs;
		}

		out.Label = raw.Label;
		out.File = raw.File;
		out.Channel = raw.Channel;
		out.Line = raw.Line;
		out.BeginUs = raw.BeginUs;
		out.EndUs = raw.EndUs;
		out.CallCount = raw.CallCount;
		out.HasData = raw.HasData;

		out.Children.resize(raw.Children.size());
		for (size_t index = 0; index < raw.Children.size(); ++index)
			MergeRealtime(out.Children[index], raw.Children[index]);
	}

	uint32_t CPUTimeScopeTree::BeginScope(const char* label, uint64_t id, ProfileChannel channel,
		const char* file, uint32_t line)
	{
		/* 帧外（启动 / 静态初始化）、非 owner 线程（worker）、通道关掉的插桩点：
		 * 静默跳过 —— 不建节点也不进栈 */
		if (!m_FrameBegun || std::this_thread::get_id() != m_OwnerThread
			|| !HasChannel(m_ChannelMask, channel))
			return INVALID_SCOPE_INDEX;

		const uint32_t parent_index = m_OpenIndices.empty()
			? INVALID_SCOPE_INDEX : m_OpenIndices.back();

		uint32_t index = INVALID_SCOPE_INDEX;
		if (parent_index == INVALID_SCOPE_INDEX)
			index = AllocNode(label, id, file, line, channel, INVALID_SCOPE_INDEX);  /* 帧根 */
		else
		{
			index = FindChild(parent_index, id);
			if (index == INVALID_SCOPE_INDEX)
				index = AllocNode(label, id, file, line, channel, parent_index);
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

	void CPUTimeScopeTree::EndScope(uint32_t node_index)
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

	uint32_t CPUTimeScopeTree::AllocNode(const char* label, uint64_t id, const char* file, uint32_t line,
		ProfileChannel channel, uint32_t parent_index)
	{
		if (m_UsedNodeCount >= m_ScopeNodes.size())
			m_ScopeNodes.emplace_back();

		const uint32_t index = m_UsedNodeCount++;
		ScopeNode& node = m_ScopeNodes[index];
		node.Label = label;
		node.Id = id;
		node.File = file;
		node.Line = line;
		node.Channel = channel;
		node.ParentIndex = parent_index;
		node.ChildrenIndices.clear();
		node.AccumulatedUs = 0.0;
		node.BeginUs = node.EndUs = node.EntryUs = 0.0;
		node.ActiveCount = 0;
		node.CallCount = 0;

		if (parent_index != INVALID_SCOPE_INDEX)
			m_ScopeNodes[parent_index].ChildrenIndices.emplace_back(index);
		return index;
	}

	uint32_t CPUTimeScopeTree::FindChild(uint32_t parent_index, uint64_t id) const
	{
		for (const uint32_t child_index : m_ScopeNodes[parent_index].ChildrenIndices)
		{
			if (m_ScopeNodes[child_index].Id == id)
				return child_index;
		}
		return INVALID_SCOPE_INDEX;
	}

	double CPUTimeScopeTree::ElapsedUs() const
	{
		return std::chrono::duration<double, std::micro>(
			std::chrono::steady_clock::now() - m_FrameStart).count();
	}

	/* 递归结算（先父后子）：EMA 只对有效样本累积；节点标签换位（结构变化）时
	 * 整棵子树作废重来，避免把不同函数的值混在一起 */
	void CPUTimeScopeTree::SettleRealtime(CPUTimeScopeNode& out, uint32_t index) const
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
		out.File = raw.File;
		out.Channel = raw.Channel;
		out.Line = raw.Line;
		out.BeginUs = raw.BeginUs;
		out.EndUs = raw.EndUs;
		out.CallCount = raw.CallCount;

		out.Children.resize(raw.ChildrenIndices.size());
		for (size_t i = 0; i < raw.ChildrenIndices.size(); ++i)
			SettleRealtime(out.Children[i], raw.ChildrenIndices[i]);
	}

	/* 原始值结算（录制版；逐帧回放要真值，不做任何平滑） */
	void CPUTimeScopeTree::SettleRaw(CPUTimeScopeNode& out, uint32_t index) const
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
		out.File = raw.File;
		out.Channel = raw.Channel;
		out.Line = raw.Line;
		out.CallCount = raw.CallCount;
		out.HasData = true;

		out.Children.resize(raw.ChildrenIndices.size());
		for (size_t i = 0; i < raw.ChildrenIndices.size(); ++i)
			SettleRaw(out.Children[i], raw.ChildrenIndices[i]);
	}
}
