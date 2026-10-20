#pragma once
#include "DependencyGraph.h"
#include "FrameGraphResourceHandle.h"

namespace Helios
{
	class FrameGraph;
	class RenderPassNode;

	/* 渲染资源节点类：
	 * 仅持有在FrameGraph中对应的Handle，不持有实际资源数据（实际资源数据由Resource类持有）；
	 * 该节点中存储与当前资源节点相关的输入/输出连线；
	 * 目前一个资源节点可对应多个使用Pass（作为当前资源节点的输出）；
	 * 但仅支持一个写入Pass（作为当前资源节点的输入）。
	 */
	class HELIOS_API RenderResourceNode : public DependencyGraph::Node
	{
	public:
		RenderResourceNode(const std::string& name, FrameGraph& frame_graph, FrameGraphResourceHandle handle);
		~RenderResourceNode() override;

		/* 获取当前资源的Handle */
		[[nodiscard]] const FrameGraphResourceHandle& GetResourceHandle() const { return m_ResourceHandle; }
		/* 获取资源名 */
		[[nodiscard]] const std::string& GetDebugName() const { return DebugName; }

		/* 增加使用节点 */
		void SetIncomingConnection(const SharedPtr<DependencyGraph::Connection>& connection) { m_pIncomingConnection = connection; }
		void AddOutgoingConnection(const SharedPtr<DependencyGraph::Connection>& connection) { m_OutgoingConnections.emplace_back(connection); }

		/* 获取资源的作为输入/输出的使用情况 */
		[[nodiscard]] bool HasOutgoingConnection() const { return !m_OutgoingConnections.empty(); }
		[[nodiscard]] bool HasIncomingConnection() const { return m_pIncomingConnection != nullptr; }

		/* 从节点的角度出发，获取与资源之间的连线 */
		[[nodiscard]] SharedPtr<DependencyGraph::Connection> GetOutgoingConnectionOfPassNode(const SharedPtr<RenderPassNode>& node) const;
		[[nodiscard]] SharedPtr<DependencyGraph::Connection> GetIncomingConnectionOfPassNode(const SharedPtr<RenderPassNode>& node) const;

		/* 更新资源的Usage */
		void UpdateResourceUsage();

		/* 销毁 */
		void Destroy();

	private:
		/* 输出可视化设置 */
		[[nodiscard]] std::string Graphvizify() const override;
		[[nodiscard]] std::string GraphvizifyConnectionColor() const override;

		/* 资源句柄，一个句柄对应FrameGraph中的一个资源 */
		FrameGraphResourceHandle m_ResourceHandle;
		/* 所属的FrameGraph */
		FrameGraph& m_FrameGraph;
		/* 资源的使用情况：一个资源可能被多个Pass使用，但最多只支持被1个Pass来写入 */
		std::vector<SharedPtr<DependencyGraph::Connection>> m_OutgoingConnections{};
		SharedPtr<DependencyGraph::Connection> m_pIncomingConnection{ nullptr };
	};
}
