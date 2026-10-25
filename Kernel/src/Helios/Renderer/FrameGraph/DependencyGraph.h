#pragma once

namespace Helios
{
	/* 依赖关系图 */
	class HELIOS_API DependencyGraph
	{
	public:
		/* 节点（嵌套类不继承外层类的导出属性，要单独标） */
		struct HELIOS_API Node
		{
			std::string DebugName{ "Unnamed Node" };	/* 标记名 */
			const uint32_t Index;							/* 节点索引，初始化之后就不再修改 */
			uint32_t RefCount{ 0 };							/* 当前节点被引用的次数 */
			bool IsTarget{ false };							/* 是否作为目标节点 */
			DependencyGraph& m_OwnerGraph;					/* 所属Dependency */

			Node(std::string name, DependencyGraph& graph);
			Node(Node&&) = default;
			virtual ~Node() = default;
			Node(const Node&) = delete;
			Node& operator=(const Node&) = delete;

			/* 是否为有效节点 */
			[[nodiscard]] bool IsValid() const { return IsTarget ? true : RefCount > 0; }
			/* 是否被剔除 */
			[[nodiscard]] bool IsCulled() const { return RefCount == 0; }

			/* 输出可视化文本 */
			[[nodiscard]] virtual std::string Graphvizify() const;
			/* 指定当前节点发出的连线的可视化颜色 */
			[[nodiscard]] virtual std::string GraphvizifyConnectionColor() const;
		};

		/* 连线（同上，嵌套类要单独标导出） */
		struct HELIOS_API Connection
		{
			uint32_t FromNodeIdx{ 0 };						/* 连线的起点节点 */
			uint32_t ToNodeIdx{ 0 };						/* 连线的终点节点 */

			Connection(const SharedPtr<Node>& from, const SharedPtr<Node>& to);
			virtual ~Connection() = default;
			Connection(const Connection&) = delete;
			Connection& operator=(const Connection&) = delete;
			/* 判断连线相同 */
			bool operator==(const Connection& other) const
			{
				return other.FromNodeIdx == FromNodeIdx && other.ToNodeIdx == ToNodeIdx;
			}
		};

		DependencyGraph();
		virtual ~DependencyGraph() = default;

		/* 注册一个节点到依赖图 */
		void RegisterNode(const SharedPtr<Node>& node);
		/* 注册一个连线到依赖图 */
		void RegisterConnection(const SharedPtr<Connection>& connection);

		/* 清楚所有节点和连线 */
		void Clear();
		/* 更新节点的引用计数 */
		void UpdateRefCount();

		/* 获取指定节点 */
		[[nodiscard]] SharedPtr<Node> GetNode(uint32_t node_idx) const;
		/* 获取所有节点 */
		[[nodiscard]] const std::vector<SharedPtr<Node>>& GetNodes() const { return m_Nodes; }
		/* 获取所有连线 */
		[[nodiscard]] const std::vector<SharedPtr<Connection>>& GetConnections() const { return m_Connections; }
		/* 获取指定节点的所有入射连线 */
		[[nodiscard]] std::vector<SharedPtr<Connection>> GetIncomingConnectionsOfNode(const SharedPtr<Node>& node) const;
		/* 获取指定节点的所有出射连线 */
		[[nodiscard]] std::vector<SharedPtr<Connection>> GetOutgoingConnectionsOfNode(const SharedPtr<Node>& node) const;
		/* 判断连线是否有效 */
		[[nodiscard]] bool IsConnectionValid(const SharedPtr<Connection>& connection) const;

		/* 输出可视化文本：便于调试（将输出字符复制到：http://www.webgraphviz.com/） */
		[[nodiscard]] std::string Graphvizify(const std::string& name = {}) const;

	private:
		/* 生成节点索引 */
		[[nodiscard]] uint32_t GenerateNodeIndex() const;

		/* 图中节点集合 */
		std::vector<SharedPtr<Node>> m_Nodes;
		/* 图中的连线集合 */
		std::vector<SharedPtr<Connection>> m_Connections;
	};
}
