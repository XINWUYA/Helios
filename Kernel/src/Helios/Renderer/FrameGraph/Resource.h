#pragma once
#include "DependencyGraph.h"
#include "RenderResourceNode.h"

namespace Helios
{
	class RenderPassNode;
	class FrameGraphResourceCache;

	/* 资源类基类 */
	class IResource
	{
	public:
		IResource(std::string name);
		IResource(std::string name, const SharedPtr<IResource>& parent);
		IResource(const IResource&) = delete;
		IResource& operator=(const IResource&) = delete;
		virtual ~IResource() = default;

		/* 被Pass依赖 */
		void NeedByPass(const SharedPtr<RenderPassNode>& pass_node);

		/* 资源名 */
		const std::string& GetName() const { return m_Name; }
		/* 获取引用计数 */
		uint32_t GetRefCount() const { return m_RefCount; }
		/* 获取被依赖的Pass */
		[[nodiscard]] SharedPtr<RenderPassNode> GetFirstPassNode() const { return m_FirstPassNode.lock(); }
		[[nodiscard]] SharedPtr<RenderPassNode> GetLastPassNode() const { return m_LastPassNode.lock(); }

		/* 创建资源 */
		virtual void Create() = 0;
		/* 销毁资源 */
		virtual void Destroy() = 0;
		/* 释放一条连线 */
		virtual void DestroyConnection(const SharedPtr<DependencyGraph::Connection>& connection) = 0;
		/* 获取UsageStr */
		virtual std::string GetUsageStr() const = 0;
		/* 更新资源的Usage */
		virtual void UpdateUsage(const SharedPtr<DependencyGraph::Connection>& connection) = 0;

	protected:
		/* 资源名*/
		std::string m_Name;
		/* 父资源节点 */
		SharedPtr<IResource> m_Parent{ nullptr };
		/* 资源引用计数 */
		uint32_t m_RefCount{ 0 };
		/* FrameGraph中第一次使用该资源的Pass */
		WeakPtr<RenderPassNode> m_FirstPassNode;
		/* FrameGraph中最后一次使用该资源的Pass */
		WeakPtr<RenderPassNode> m_LastPassNode;
	};

	/* 资源类 */
	template<typename ResourceType>
	class Resource : public IResource
	{
	public:
		using ResourceUsage = typename ResourceType::Usage;
		using ResourceDescriptor = typename ResourceType::Descriptor;
		using ResourceSubDescriptor = typename ResourceType::SubDescriptor;

		/* 资源节点连线 */
		struct ResourceConnection : public DependencyGraph::Connection
		{
			ResourceUsage Usage;

			ResourceConnection(const SharedPtr<DependencyGraph::Node>& from, const SharedPtr<DependencyGraph::Node>& to, ResourceUsage usage)
				: DependencyGraph::Connection(from, to), Usage(usage)
			{
			}

			~ResourceConnection() override = default;
		};

		Resource(const std::string& name, const ResourceDescriptor& desc)
			: IResource(name), m_Descriptor(desc)
		{
		}
		Resource(const std::string& name, const SharedPtr<IResource>& parent, const ResourceSubDescriptor& sub_desc)
			: IResource(name, parent), m_SubDescriptor(sub_desc)
		{
		}
		explicit Resource(const Resource&) = delete;
		~Resource() override = default;

		/* 获取资源本体 */
		const ResourceType& GetResource() const { return m_Resource; }

		/* 设置资源描述 */
		void SetDescriptor(const ResourceDescriptor& descriptor) { m_Descriptor = descriptor; }
		const ResourceDescriptor& GetDescriptor() const { return m_Descriptor; }

		/* 设置子资源描述 */
		void SetSubDescriptor(const ResourceSubDescriptor& sub_descriptor) { m_SubDescriptor = sub_descriptor; }
		const ResourceSubDescriptor& GetSubDescriptor() const { return m_SubDescriptor; }

		/* 设置资源用途 */
		void SetUsage(ResourceUsage usage) { m_Usage = usage; }
		ResourceUsage GetUsage() const { return m_Usage; }

		/* 将资源节点作为输入连接到依赖图中 */
		bool ConnectAsInput(DependencyGraph& dependency_graph, const SharedPtr<RenderPassNode>& render_pass_node, const SharedPtr<RenderResourceNode>& resource_node, ResourceUsage usage)
		{
			/* 获取当前RenderPass的输入连线 */
			auto connection = StaticPtrCast<ResourceConnection>(resource_node->GetIncomingConnectionOfPassNode(render_pass_node));
			if (connection)
			{
				connection->Usage |= usage;
			}
			else
			{
				connection = CreateSharedPtr<ResourceConnection>(resource_node, render_pass_node, usage);
				/* 将连线注册到依赖图中 */
				dependency_graph.RegisterConnection(connection);
				/* 也将连线保存到当前资源节点 */
				resource_node->AddOutgoingConnection(connection);
			}
			return true;
		}

		/* 将资源节点作为输出连接到依赖图中 */
		bool ConnectAsOutput(DependencyGraph& dependency_graph, const SharedPtr<RenderPassNode>& render_pass_node, const SharedPtr<RenderResourceNode>& resource_node, ResourceUsage usage)
		{
			/* 获取当前RenderPass的输出连线 */
			auto connection = StaticPtrCast<ResourceConnection>(resource_node->GetOutgoingConnectionOfPassNode(render_pass_node));
			if (connection)
			{
				connection->Usage |= usage;
			}
			else
			{
				connection = CreateSharedPtr<ResourceConnection>(render_pass_node, resource_node, usage);
				/* 将连线注册到依赖图中 */
				dependency_graph.RegisterConnection(connection);
				/* 也将连线保存到当前资源节点 */
				resource_node->SetIncomingConnection(connection);
			}
			return true;
		}

		/* 设置所属 FrameGraph 的跨帧资源缓存（由 FrameGraph::CreateResource 注入；
		 * 资源创建 / 销毁时带着它走，见 FrameGraphResourceCache） */
		void SetResourceCache(FrameGraphResourceCache* cache) { m_ResourceCache = cache; }

		/* 创建资源 */
		void Create() override
		{
			m_Resource.Create(m_Name, m_Descriptor, m_Usage, m_ResourceCache);
		}

		/* 销毁资源 */
		void Destroy() override
		{
			m_Resource.Destroy(m_Name, m_Descriptor, m_Usage, m_ResourceCache);
		}

		/* 释放一条连线 */
		void DestroyConnection(const SharedPtr<DependencyGraph::Connection>& connection) override
		{
			StaticPtrCast<ResourceConnection>(connection).reset();
		}

		/* 获取UsageStr */
		std::string GetUsageStr() const override
		{
			return "";//std::to_string(m_Usage);
		}

		/* 更新资源的Usage */
		void UpdateUsage(const SharedPtr<DependencyGraph::Connection>& connection) override
		{
			auto conn = StaticPtrCast<ResourceConnection>(connection);
			m_Usage |= conn->Usage;
		}

	private:
		ResourceType m_Resource{}; /* 资源本体数据 */
		ResourceDescriptor m_Descriptor{}; /* 资源描述 */
		ResourceSubDescriptor m_SubDescriptor{}; /* 子资源描述 */
		ResourceUsage m_Usage{}; /* 资源用途 */
		FrameGraphResourceCache* m_ResourceCache{ nullptr }; /* 跨帧资源缓存（可为空） */
	};
}
