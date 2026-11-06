#pragma once
#include "Blackboard.h"
#include "FrameGraphPass.h"
#include "DependencyGraph.h"
#include "FrameGraphCapture.h"
#include "FrameGraphResourceHandle.h"
#include "FrameGraphResources.h"
#include "FrameGraphResourceCache.h"
#include "Resource.h"
#include "RenderPassNode.h"
#include "RenderResourceNode.h"

namespace Helios
{
	class FrameGraphBuilder;
	class RenderResourceNode;
	struct FrameBufferDesc;

	/* FrameGraph类 */
	class HELIOS_API FrameGraph final
	{
	public:
		explicit FrameGraph(const std::string& name);
		FrameGraph(FrameGraph&) = delete;
		FrameGraph& operator=(const FrameGraph&) = delete;
		~FrameGraph();

		/* 添加一个FrameGraphPass：
		 * SetupFunc将被立即执行；
		 * ExecuteFunc将在执行整个FrameGraph时才会执行；
		 */
		template<typename Data, typename SetupFunc, typename ExecuteFunc>
		SharedPtr<FrameGraphPass<Data, ExecuteFunc>> AddPass(const std::string& name, SetupFunc setup_func, ExecuteFunc&& execute_func);

		/* 创建资源 */
		template<typename ResourceType>
		FrameGraphResourceHandleTyped<ResourceType> CreateResource(const std::string& name, const typename ResourceType::Descriptor& desc = {})
		{
			auto resource = CreateSharedPtr<Resource<ResourceType>>(name, desc);
			/* 注入资源池：Create 时取同描述的空闲纹理，Destroy 时还回去 */
			resource->SetResourceCache(&m_ResourceCache);
			return FrameGraphResourceHandleTyped<ResourceType>(AddResourceInternal(resource));
		}

		/* 导入外部纹理：Create 不取池、Destroy 不还池，生命周期与内容都归导入方。
		 * 跨帧的时域历史只能这样进图 —— 池子按描述分池，历史纹理随时会被别的 Pass 抢走。 */
		FrameGraphResourceHandleTyped<FrameGraphTexture> ImportTexture(const std::string& name,
			const SharedPtr<class DeviceTexture>& texture);

		/* 导入外部计算缓冲：规则同 ImportTexture。跨帧保留的剔除结果、调色板都走这条 */
		FrameGraphResourceHandleTyped<FrameGraphBuffer> ImportBuffer(const std::string& name,
			const SharedPtr<class DeviceStorageBuffer>& buffer);

		/* 为RenderPass绑定输入资源 */
		template<typename ResourceType>
		void BindInputResource(const SharedPtr<RenderPassNode>& render_pass_node, FrameGraphResourceHandleTyped<ResourceType> handle, typename ResourceType::Usage usage = ResourceType::DefaultReadUsage)
		{
			if (!IsHandleValid(handle))
				return;

			/* 将资源节点作为输入连接到依赖图中 */
			auto resource = StaticPtrCast<Resource<ResourceType>>(m_Resources[handle.GetIndex()]);
			resource->ConnectAsInput(m_DependencyGraph, render_pass_node, m_ResourceNodes[handle.GetIndex()], usage);
		}

		/* 为RenderPass绑定输出资源 */
		template<typename ResourceType>
		void BindOutputResource(const SharedPtr<RenderPassNode>& render_pass_node, FrameGraphResourceHandleTyped<ResourceType> handle, typename ResourceType::Usage usage = ResourceType::DefaultWriteUsage)
		{
			if (!IsHandleValid(handle))
				return;

			/* 将资源节点作为输出连接到依赖图中 */
			auto resource = StaticPtrCast<Resource<ResourceType>>(m_Resources[handle.GetIndex()]);
			resource->ConnectAsOutput(m_DependencyGraph, render_pass_node, m_ResourceNodes[handle.GetIndex()], usage);
		}

		/* 获取资源 */
		[[nodiscard]] SharedPtr<IResource> GetResource(FrameGraphResourceHandle handle);
		/* 获取资源节点 */
		[[nodiscard]] SharedPtr<RenderResourceNode> GetRenderResourceNode(FrameGraphResourceHandle handle);
		/* 获取依赖关系图 */
		[[nodiscard]] DependencyGraph& GetDependencyGraph() { return m_DependencyGraph; }
		/* 获取Blackboard */
		[[nodiscard]] Blackboard& GetBlackboard() { return m_Blackboard; }
		[[nodiscard]] const Blackboard& GetBlackboard() const { return m_Blackboard; }
		//const DependencyGraph& GetDependencyGraph() const { return m_DependencyGraph; }

		/* 检查一个资源节点是否有效 */
		[[nodiscard]] bool IsHandleValid(FrameGraphResourceHandle handle) const;
		/* 标记导出：帧结束后还要读的资源（黑板里的句柄会自动导出） */
		void ExportResource(FrameGraphResourceHandle handle);
		/* 判断一个资源连线是否有效 */
		[[nodiscard]] bool IsConnectionValid(const SharedPtr<DependencyGraph::Connection>& connection) const;
		/* 检查一个FrameGraphPass节点是否被优化掉 */
		[[nodiscard]] bool IsPassCulled(const SharedPtr<IFrameGraphPass>& pass) const;

		/* 构建FrameGraph */
		void Build() noexcept;
		/* 执行 */
		void Execute() noexcept;
		/* 重置 */
		void Reset() noexcept;

		/* 获取（或跨帧复用）一个 Pass 的 FrameBuffer：名称 + 描述不变时不再重新分配
		 * RenderPassDescriptor 与附件（见 FrameGraphResourceCache）。 */
		[[nodiscard]] SharedPtr<DeviceFrameBuffer> AcquirePassFrameBuffer(const std::string& pass_name, const FrameBufferDesc& desc);

		/* 诊断：跨帧资源缓存的创建 / 复用统计 */
		[[nodiscard]] const FrameGraphResourceCache::Stats& GetResourceCacheStats() const { return m_ResourceCache.GetStats(); }

		/* 逐 Pass 抓取设施（中间渲染结果快照，供可视化面板；默认禁用、零开销） */
		[[nodiscard]] FrameGraphCapture& GetCapture() { return m_Capture; }

		/* 导出依赖图 http://dreampuf.github.io/GraphvizOnline/ */
		void ExportGraphviz(const std::string& path);

	private:
		/* 剔除掉无效的RenderPassNode */
		void CullRenderPassNodes();

		/* 抓取一个 Pass 的颜色附件（在其执行完、资源销毁之前调用；见 FrameGraphCapture） */
		void CapturePassOutputs(RenderPassNode& pass_node);

		/* 添加资源到资源表 */
		FrameGraphResourceHandle AddResourceInternal(const SharedPtr<IResource>& resource);

		/* 释放FrameGraph */
		void Destroy();

		/* 标记名 */
		std::string m_DebugName{ "Unnamed FrameGraph" };
		/* RenderPassNode列表，包含当前FrameGraph所有Pass节点，再Cull阶段才会剔除其中无效的节点 */
		std::vector<SharedPtr<RenderPassNode>> m_RenderPassNodes{};
		/* 资源表与资源节点表，下标就是资源句柄的 Index */
		std::vector<SharedPtr<IResource>> m_Resources{};
		std::vector<SharedPtr<RenderResourceNode>> m_ResourceNodes{};
		/* Blackboard: 资源名到资源的映射 */
		Blackboard m_Blackboard{};
		/* Pass依赖关系图 */
		DependencyGraph m_DependencyGraph;
		/* 最后一个有效的RenderPassNode */
		std::vector<SharedPtr<RenderPassNode>>::iterator m_LastValidRenderPassNodeIter;
		/* transient 资源池：Reset 只清渲染图结构，池里的 GPU 资源按描述跨 Pass、跨帧复用 */
		FrameGraphResourceCache m_ResourceCache;
		/* 逐 Pass 抓取设施（中间渲染结果快照）：启用时在每个 Pass 执行后拷贝其颜色附件 */
		FrameGraphCapture m_Capture;
	};

	/* FrameGraph Builder类
	 * 每个RenderPass对应对应一个Builder
	 * 用于创建或绑定当前RenderPass所需的资源
	 */
	class HELIOS_API FrameGraphBuilder
	{
	public:
		explicit FrameGraphBuilder(FrameGraphBuilder&) = delete;
		FrameGraphBuilder& operator=(const FrameGraphBuilder&) = delete;
		~FrameGraphBuilder() = default;

		/* 根据描述创建RenderPass */
		uint32_t CreateRenderPass(const std::string& name, const FrameGraphPassInfo::Descriptor& desc = {});

		/* 根据描述创建一个纹理资源 */
		FrameGraphResourceHandleTyped<FrameGraphTexture> CreateTexture(const std::string& name, const FrameGraphTexture::Descriptor& desc = {});

		/* 根据描述创建一个计算缓冲资源 */
		FrameGraphResourceHandleTyped<FrameGraphBuffer> CreateBuffer(const std::string& name, const FrameGraphBuffer::Descriptor& desc = {});

		/* 为当前RenderPass绑定输入资源 */
		template<typename ResourceType>
		void BindInputResource(FrameGraphResourceHandleTyped<ResourceType> handle, typename ResourceType::Usage usage = ResourceType::DefaultReadUsage)
		{
			m_FrameGraph.BindInputResource<ResourceType>(m_pRenderPassNode, handle, usage);
		}

		/* 为当前RenderPass绑定输出资源 */
		template<typename ResourceType>
		void BindOutputResource(FrameGraphResourceHandleTyped<ResourceType> handle, typename ResourceType::Usage usage = ResourceType::DefaultWriteUsage)
		{
			m_FrameGraph.BindOutputResource<ResourceType>(m_pRenderPassNode, handle, usage);
		}

		/* 使当前节点不会被优化掉，即使没有没使用到 */
		void AsSideEffect(bool value = true);

		/* 标记成计算 Pass：只派发、不画附件 —— 不绑默认渲染目标，也不参与附件抓取 */
		void AsComputePass(bool value = true);

		/* 根据资源Handle在FrameGraph中获取资源名 */
		[[nodiscard]] const std::string& GetResourceName(FrameGraphResourceHandle handle) const;


	private:
		FrameGraphBuilder(FrameGraph& frame_graph, const SharedPtr<RenderPassNode>& render_pass_node);

		/* 当前Builder所属的FrameGraph */
		FrameGraph& m_FrameGraph;
		/* 对应的RenderPass */
		SharedPtr<RenderPassNode> m_pRenderPassNode{ nullptr };

		friend class FrameGraph;
	};

	/* FrameGraph::AddPass implementation */
	template<typename Data, typename SetupFunc, typename ExecuteFunc>
	SharedPtr<FrameGraphPass<Data, ExecuteFunc>> FrameGraph::AddPass(const std::string& name, SetupFunc setup_func, ExecuteFunc&& execute_func)
	{
		auto frame_graph_pass = CreateSharedPtr<FrameGraphPass<Data, ExecuteFunc>>(std::forward<ExecuteFunc>(execute_func));
		auto render_pass_node = CreateSharedPtr<RenderPassNode>(name, *this, frame_graph_pass);
		/* 将Pass节点注册到依赖图中 */
		m_DependencyGraph.RegisterNode(render_pass_node);

		frame_graph_pass->SetRenderPassNode(render_pass_node);
		m_RenderPassNodes.emplace_back(render_pass_node);

		FrameGraphBuilder builder(*this, render_pass_node);

		/* 执行Setup阶段 */
		setup_func(builder, const_cast<Data&>(frame_graph_pass->GetData()));

		return frame_graph_pass;
	}

}
