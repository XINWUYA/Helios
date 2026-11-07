#include "Pch.h"
#include "FrameGraph.h"
#include "FrameGraphResourceCache.h"
#include "RenderPassNode.h"
#include "RenderResourceNode.h"
#include "Helios/Renderer/Renderer.h"
#include "Helios/VirtualDevice/DeviceFrameBuffer.h"
#include "Helios/VirtualDevice/DeviceTexture.h"
#include "Helios/VirtualDevice/DeviceBuffer.h"

namespace Helios
{
	/* 根据描述创建一个RenderPass */
	uint32_t FrameGraphBuilder::CreateRenderPass(const std::string& name, const FrameGraphPassInfo::Descriptor& desc)
	{
		return m_pRenderPassNode->CreateRenderPassData(name, desc);
	}

	/* 根据描述创建一个纹理资源 */
	FrameGraphResourceHandleTyped<FrameGraphTexture> FrameGraphBuilder::CreateTexture(const std::string& name, const FrameGraphTexture::Descriptor& desc)
	{
		return m_FrameGraph.CreateResource<FrameGraphTexture>(name, desc);
	}

	FrameGraphResourceHandleTyped<FrameGraphBuffer> FrameGraphBuilder::CreateBuffer(const std::string& name, const FrameGraphBuffer::Descriptor& desc)
	{
		return m_FrameGraph.CreateResource<FrameGraphBuffer>(name, desc);
	}

	/* 使当前节点不会被优化掉，即使没有没使用到 */
	void FrameGraphBuilder::AsSideEffect(bool value)
	{
		m_pRenderPassNode->IsTarget = value;
	}

	/* 标记成计算 Pass */
	void FrameGraphBuilder::AsComputePass(bool value)
	{
		m_pRenderPassNode->IsComputePass = value;
	}

	/* 根据资源Handle在FrameGraph中获取资源名 */
	const std::string& FrameGraphBuilder::GetResourceName(FrameGraphResourceHandle handle) const
	{
		return m_FrameGraph.GetResource(handle)->GetName();
	}

	FrameGraphBuilder::FrameGraphBuilder(FrameGraph& frame_graph, const SharedPtr<RenderPassNode>& render_pass_node)
		: m_FrameGraph(frame_graph), m_pRenderPassNode(render_pass_node)
	{
	}

	FrameGraph::FrameGraph(const std::string& name)
		: m_DebugName(name)
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);
		m_RenderPassNodes.reserve(100);
		m_Resources.reserve(128);
		m_ResourceNodes.reserve(128);
	}

	FrameGraph::~FrameGraph()
	{
		Destroy();
	}

	/* 获取资源 */
	SharedPtr<IResource> FrameGraph::GetResource(FrameGraphResourceHandle handle)
	{
		if (!IsHandleValid(handle))
			return nullptr;

		return m_Resources[handle.GetIndex()];
	}

	/* 获取资源节点 */
	SharedPtr<RenderResourceNode> FrameGraph::GetRenderResourceNode(FrameGraphResourceHandle handle)
	{
		if (!IsHandleValid(handle))
			return nullptr;

		return m_ResourceNodes[handle.GetIndex()];
	}

	/* 检查一个资源句柄是否指向本帧的资源 */
	bool FrameGraph::IsHandleValid(FrameGraphResourceHandle handle) const
	{
		return handle.IsInitialized() && handle.GetIndex() < m_Resources.size();
	}

	/* 标记导出 */
	void FrameGraph::ExportResource(FrameGraphResourceHandle handle)
	{
		if (IsHandleValid(handle))
			m_Resources[handle.GetIndex()]->SetExported(true);
	}

	/* 判断一个资源连线是否有效 */
	bool FrameGraph::IsConnectionValid(const SharedPtr<DependencyGraph::Connection>& connection) const
	{
		return m_DependencyGraph.IsConnectionValid(connection);
	}

	/* 检查一个FrameGraphPass节点是否被优化掉 */
	bool FrameGraph::IsPassCulled(const SharedPtr<IFrameGraphPass>& pass) const
	{
		return pass->GetRenderPassNode()->IsCulled();
	}

	/* 构建FrameGraph */
	void FrameGraph::Build() noexcept
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

		CullRenderPassNodes();

		/* 黑板里的句柄会在图外被读，统一按导出处理 */
		for (const auto& [name, handle] : m_Blackboard.GetEntries())
			ExportResource(handle);

		/* 仅对有效的RenderPassNode执行构建 */
		auto iter_current = m_RenderPassNodes.begin();
		while (iter_current != m_LastValidRenderPassNodeIter)
		{
			auto current_node = *iter_current;

			/* 输入涉及资源 */
			for (const auto& connection : m_DependencyGraph.GetIncomingConnectionsOfNode(current_node))
			{
				const auto from_node = StaticPtrCast<RenderResourceNode>(m_DependencyGraph.GetNode(connection->FromNodeIdx));
				current_node->RegisterResourceHandle(from_node->GetResourceHandle());
			}

			/* 输出涉及资源 */
			for (const auto& connection : m_DependencyGraph.GetOutgoingConnectionsOfNode(current_node))
			{
				const auto to_node = StaticPtrCast<RenderResourceNode>(m_DependencyGraph.GetNode(connection->ToNodeIdx));
				current_node->RegisterResourceHandle(to_node->GetResourceHandle());
			}
			/* 构建当前节点 */
			current_node->Build();

			/* 下一个 */
			++iter_current;
		}

		/* 确定资源的构建和销毁时机 */
		for (const auto& resource : m_Resources)
		{
			if (resource->GetRefCount() > 0)
			{
				auto first_pass_node = resource->GetFirstPassNode();
				auto last_pass_node = resource->GetLastPassNode();

				if (first_pass_node && last_pass_node)
				{
					ASSERT(first_pass_node->IsValid() && last_pass_node->IsValid());
					/* 资源要在第一次使用时创建 */
					first_pass_node->GetResourcesNeedPrepared().emplace_back(resource);
					/* 在最后一次使用完时销毁 */
					last_pass_node->GetResourcesNeedDestroy().emplace_back(resource);
				}
			}
		}

		/* 在Cull之后再更新资源的Usage */
		for (auto& resource_node : m_ResourceNodes)
			resource_node->UpdateResourceUsage();
	}

	/* 执行FrameGraph */
	void FrameGraph::Execute() noexcept
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

		/* 帧入口：推进跨帧缓存的帧号并做淘汰 —— 即使本帧没有 Pass 也要发生，
		 * 否则空图期间缓存永不收缩 */
		m_ResourceCache.BeginFrame();
		m_Capture.BeginFrame();

		if (m_RenderPassNodes.empty())
		{
			m_Capture.EndFrame();
			return;
		}

		/* 仅执行有效的RenderPassNodes */
		auto iter_current = m_RenderPassNodes.begin();
		while (iter_current != m_LastValidRenderPassNodeIter)
		{
			auto& current_node = *iter_current;

			/* 准备当前节点所需的资源 */
			for (auto& resource : current_node->GetResourcesNeedPrepared())
				resource->Create();

			/* 执行Pass */
			FrameGraphResources resources(*this, *current_node);
			current_node->Execute(resources);

			/* 抓取本 Pass 的附件（颜色 + 深度；中间渲染结果快照）：必须在销毁步骤
			 * 之前 —— 附件可能正是本节点的"最后一次使用"；叠加型 Pass 的后续修改
			 * 不会回溯影响这里留的快照（见 FrameGraphCapture） */
			CapturePassOutputs(*current_node);

			/* 最后一次用完就还回资源池，同帧后面的 Pass 可以接着用这块显存；
			 * 导出的资源帧后还要读，留到下一帧 Reset 再还 */
			for (auto& resource : current_node->GetResourcesNeedDestroy())
			{
				if (!resource->IsExported())
					resource->Destroy();
			}
			/* 下一个 */
			++iter_current;
		}

		m_Capture.EndFrame();
	}

	/* 获取（或跨帧复用）一个 Pass 的 FrameBuffer */
	SharedPtr<DeviceFrameBuffer> FrameGraph::AcquirePassFrameBuffer(const std::string& pass_name, const FrameBufferDesc& desc)
	{
		return m_ResourceCache.AcquireFrameBuffer(pass_name + "_FrameBuffer", desc);
	}

	/* 导入外部纹理：资源本体直接指向调用方给的那张，并打上"别还池"的标记 */
	FrameGraphResourceHandleTyped<FrameGraphTexture> FrameGraph::ImportTexture(const std::string& name,
		const SharedPtr<DeviceTexture>& texture)
	{
		auto resource = CreateSharedPtr<Resource<FrameGraphTexture>>(name, FrameGraphTexture::Descriptor{});
		resource->GetResourceMutable().Texture = texture;
		resource->GetResourceMutable().Imported = true;
		return FrameGraphResourceHandleTyped<FrameGraphTexture>(AddResourceInternal(resource));
	}

	FrameGraphResourceHandleTyped<FrameGraphBuffer> FrameGraph::ImportBuffer(const std::string& name,
		const SharedPtr<DeviceStorageBuffer>& buffer)
	{
		const FrameGraphBuffer::Descriptor desc{ buffer != nullptr ? buffer->GetDataSize() : 0u };
		auto resource = CreateSharedPtr<Resource<FrameGraphBuffer>>(name, desc);
		resource->GetResourceMutable().Buffer = buffer;
		resource->GetResourceMutable().Imported = true;
		return FrameGraphResourceHandleTyped<FrameGraphBuffer>(AddResourceInternal(resource));
	}

	/* 重置：黑板跟着清空，上一帧的句柄不能留到这一帧 */
	void FrameGraph::Reset() noexcept
	{
		Destroy();
		m_Blackboard.Clear();
	}

	/* 导出依赖图 */
	void FrameGraph::ExportGraphviz(const std::string& path)
	{
		const std::string& str = m_DependencyGraph.Graphvizify("FrameGraph");

		std::ofstream file_out(path.c_str());
		file_out << str;
		file_out.close();
	}

	/* 抓取一个 Pass 的附件（颜色在前、深度在后）：把附件句柄解析成 (资源名, 纹理) 交给抓取设施。
	 * 调用点在 Pass 执行后、资源销毁前（这时附件内容 = 该阶段刚结束的状态）；多组 RenderPassData
	 * 按序全部收集。 */
	void FrameGraph::CapturePassOutputs(RenderPassNode& pass_node)
	{
		if (!m_Capture.IsEnabled())
			return;

		std::vector<std::pair<std::string, SharedPtr<DeviceTexture>>> outputs;

		for (uint32_t pass_data_index = 0;; ++pass_data_index)
		{
			const SharedPtr<RenderPassData> render_pass_data = pass_node.GetRenderPassData(pass_data_index);
			if (render_pass_data == nullptr)
				break;

			for (uint32_t i = 0; i < MAX_COLOR_ATTACHMENT_NUM; ++i)
			{
				const FrameGraphResourceHandle& handle = render_pass_data->ValidAttachments[i];
				if (!handle.IsInitialized())
					continue;

				const auto resource = StaticPtrCast<Resource<FrameGraphTexture>>(GetResource(handle));
				if (resource == nullptr || resource->GetResource().Texture == nullptr)
					continue;

				outputs.emplace_back(resource->GetName(), resource->GetResource().Texture);
			}

			const FrameGraphResourceHandle& depth_handle =
				render_pass_data->ValidAttachments[FrameGraphPassInfo::DEPTH_ATTACHMENT_IDX];
			if (depth_handle.IsInitialized())
			{
				const auto resource = StaticPtrCast<Resource<FrameGraphTexture>>(GetResource(depth_handle));
				if (resource != nullptr && resource->GetResource().Texture != nullptr)
					outputs.emplace_back(resource->GetName(), resource->GetResource().Texture);
			}
		}

		/* 无附件的 Pass = 渲染到默认目标（前向 ScenePass 等）：把窗口画布快照作为它的
		 * 一路输出（"DefaultTarget"）—— 纹理由 RenderAPI 持有、跨帧复用；headless /
		 * 无窗口时拿到 nullptr，静默跳过（面板里该车道保持只有标题） */
		if (outputs.empty())
		{
			/* 计算 Pass 没有画面：别拿默认目标冒充它的输出 */
			if (pass_node.IsComputePass)
				return;

			const SharedPtr<RenderAPI>& render_api = Renderer::GetRenderAPI();
			if (render_api != nullptr)
			{
				bool top_down = false;
				if (SharedPtr<DeviceTexture> snapshot = render_api->AcquireDefaultTargetSnapshot(top_down))
					m_Capture.CaptureSnapshot(pass_node.GetDebugName(), "DefaultTarget", snapshot, top_down);
			}
			return;
		}

		m_Capture.CapturePass(pass_node.GetDebugName(), outputs);
	}

	/* 剔除掉无效的RenderPassNode */
	void FrameGraph::CullRenderPassNodes()
	{
		PROFILE_CPU_FUNCTION_CH(ProfileChannel::Render);

		/* 更新依赖图中的引用计数 */
		m_DependencyGraph.UpdateRefCount();

		/* 重新排序并筛选出有效的RenderPassNodes */
		m_LastValidRenderPassNodeIter = std::stable_partition(m_RenderPassNodes.begin(), m_RenderPassNodes.end(),
			[](auto const& render_pass_node)
			{
				return render_pass_node->IsValid();
			});
	}

	/* 添加资源到资源表 */
	FrameGraphResourceHandle FrameGraph::AddResourceInternal(const SharedPtr<IResource>& resource)
	{
		const FrameGraphResourceHandle resource_handle(static_cast<uint16_t>(m_Resources.size()));
		m_Resources.push_back(resource);

		/* 新增一个RenderResourceNode，并添加到依赖图 */
		auto resource_node = CreateSharedPtr<RenderResourceNode>(resource->GetName(), *this, resource_handle);
		m_DependencyGraph.RegisterNode(resource_node);
		m_ResourceNodes.push_back(resource_node);

		return resource_handle;
	}

	/* 释放FrameGraph */
	void FrameGraph::Destroy()
	{
		for (const auto& render_pass_node: m_RenderPassNodes)
			render_pass_node->Destroy();
		m_RenderPassNodes.clear();

		/* 必须先销毁ResourceNode，才能销毁Resource */
		for (const auto& resource_node : m_ResourceNodes)
			resource_node->Destroy();
		m_ResourceNodes.clear();

		for (const auto& resource : m_Resources)
			resource->Destroy();
		m_Resources.clear();

		m_DependencyGraph.Clear();
	}
}
