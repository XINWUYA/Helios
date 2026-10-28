#include "Pch.h"
#include "DependencyGraph.h"

namespace Helios
{
	DependencyGraph::Node::Node(std::string name, DependencyGraph& graph)
		: DebugName(std::move(name)), Index(graph.GenerateNodeIndex()), m_OwnerGraph(graph)
	{
	}

	/* 输出可视化文本 */
	std::string DependencyGraph::Node::Graphvizify() const
	{
		std::string result_str;
		result_str.reserve(128);
		result_str.append("[label=\"");
		result_str.append(DebugName);
		result_str.append("\\nrefs: ");
		result_str.append(std::to_string(RefCount));
		result_str.append(", id: ");
		result_str.append(std::to_string(Index));
		result_str.append("\", style=filled, fillcolor=");
		result_str.append(IsValid() ? "skyblue" : "skyblue4");
		result_str.append("]");
		result_str.shrink_to_fit();

		return  result_str;
	}

	/* 指定当前节点发出的连线的可视化颜色 */
	std::string DependencyGraph::Node::GraphvizifyConnectionColor() const
	{
		return "darkolivegreen";
	}

	DependencyGraph::Connection::Connection(const SharedPtr<Node>& from, const SharedPtr<Node>& to)
		: FromNodeIdx(from->Index), ToNodeIdx(to->Index)
	{
	}

	DependencyGraph::DependencyGraph()
	{
		m_Nodes.reserve(64);
		m_Connections.reserve(128);
		m_IncomingConnections.reserve(64);
		m_OutgoingConnections.reserve(64);
	}

	/* 清楚所有节点和连线 */
	void DependencyGraph::Clear()
	{
		PROFILE_FUNCTION();

		m_Nodes.clear();
		m_Connections.clear();
		m_IncomingConnections.clear();
		m_OutgoingConnections.clear();
	}

	/* 更新节点的引用计数 */
	void DependencyGraph::UpdateRefCount()
	{
		PROFILE_FUNCTION();

		/* 更新节点的引用计数 */
		for (const auto& connection : m_Connections)
		{
			m_Nodes[connection->FromNodeIdx]->RefCount++;
		}

		/* 筛选出无效的节点 */
		std::vector<Node*> invalid_nodes;
		invalid_nodes.reserve(m_Nodes.size());
		for (const auto& node : m_Nodes)
		{
			if (!node->IsValid())
				invalid_nodes.push_back(node.get());
		}

		/* 对于无效的节点，减少其入射节点的引用计数 */
		while (!invalid_nodes.empty())
		{
			Node* node = invalid_nodes.back();
			invalid_nodes.pop_back();

			for (const auto& connection : m_IncomingConnections[node->Index])
			{
				Node* from_node = m_Nodes[connection->FromNodeIdx].get();
				from_node->RefCount--;

				if (!from_node->IsValid())
					invalid_nodes.push_back(from_node);
			}
		}
	}

	/* 获取指定节点 */
	SharedPtr<DependencyGraph::Node> DependencyGraph::GetNode(uint32_t node_idx) const
	{
		if (node_idx >= m_Nodes.size())
			return nullptr;

		return m_Nodes[node_idx];
	}

	/* 获取指定节点的所有入射连线 */
	const std::vector<SharedPtr<DependencyGraph::Connection>>& DependencyGraph::GetIncomingConnectionsOfNode(const SharedPtr<Node>& node) const
	{
		static const std::vector<SharedPtr<Connection>> empty;
		return node->Index < m_IncomingConnections.size() ? m_IncomingConnections[node->Index] : empty;
	}

	/* 获取指定节点的所有出射连线 */
	const std::vector<SharedPtr<DependencyGraph::Connection>>& DependencyGraph::GetOutgoingConnectionsOfNode(const SharedPtr<Node>& node) const
	{
		static const std::vector<SharedPtr<Connection>> empty;
		return node->Index < m_OutgoingConnections.size() ? m_OutgoingConnections[node->Index] : empty;
	}

	/* 判断连线是否有效 */
	bool DependencyGraph::IsConnectionValid(const SharedPtr<Connection>& connection) const
	{
		return m_Nodes[connection->FromNodeIdx]->IsValid() && m_Nodes[connection->ToNodeIdx]->IsValid();
	}

	/* 输出可视化文本：便于调试（将输出字符复制到：http://www.webgraphviz.com/） */
	std::string DependencyGraph::Graphvizify(const std::string& name) const
	{
		std::ostringstream out_stream;

		const char* graph_name = name.empty() ? name.c_str() : "graph";
		out_stream << "digraph \"" << graph_name << "\" {\n";
		out_stream << "rankdir = LR\n";
		out_stream << "bgcolor = black\n";
		out_stream << "node [shape=rectangle, fontname=\"helvetica\", fontsize=10]\n\n";

		/* 各节点的内容 */
		for (const auto& node : m_Nodes)
		{
			std::string s = node->Graphvizify();
			out_stream << "\"N" << node->Index << "\" " << s.c_str() << "\n";
		}

		/* 连线 */
		out_stream << "\n";
		for (const auto& node : m_Nodes)
		{
			auto connections = GetOutgoingConnectionsOfNode(node);

			/* 重新排序，并筛选有效的连线 */
			auto iter_partition = std::partition(connections.begin(), connections.end(),
				[this](auto const& edge)
				{
					return IsConnectionValid(edge);
				});

			std::string s = node->GraphvizifyConnectionColor();

			auto iter_current = connections.begin();

			/* 先画有效的连线 */
			if (iter_current != iter_partition)
			{
				out_stream << "N" << node->Index << " -> { ";

				while (iter_current != iter_partition)
				{
					/*const auto* iter_ref = GetNode((*iter_current++)->ToNodeIdx);
					out_stream << "N" << iter_ref->Index << " ";*/
					out_stream << "N" << (*iter_current++)->ToNodeIdx << " ";
				}

				out_stream << "} [color=" << s.c_str() << "2]\n";
			}

			/* 再画无效的连线 */
			if (iter_current != connections.end())
			{
				out_stream << "N" << node->Index << " -> { ";

				while (iter_current != connections.end())
				{
					/*Node const* iter_next = GetNode((*iter_current++)->ToNodeIdx);
					out_stream << "N" << iter_next->Index << " ";*/
					out_stream << "N" << (*iter_current++)->ToNodeIdx << " ";

				}
				out_stream << "} [color=" << s.c_str() << "4 style=dashed]\n";
			}
		}

		out_stream << "}" << std::endl;

		return out_stream.str();
	}

	/* 注册一个节点到依赖图 */
	void DependencyGraph::RegisterNode(const SharedPtr<Node>& node)
	{
		m_Nodes.emplace_back(node);
		m_IncomingConnections.resize(m_Nodes.size());
		m_OutgoingConnections.resize(m_Nodes.size());
	}

	/* 注册一个连线到依赖图 */
	void DependencyGraph::RegisterConnection(const SharedPtr<Connection>& connection)
	{
		m_Connections.emplace_back(connection);
		m_OutgoingConnections[connection->FromNodeIdx].push_back(connection);
		m_IncomingConnections[connection->ToNodeIdx].push_back(connection);
	}

	/* 生成节点索引 */
	uint32_t DependencyGraph::GenerateNodeIndex() const
	{
		return static_cast<uint32_t>(m_Nodes.size());
	}
}
