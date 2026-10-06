#include "Pch.h"
#include "RenderPassNode.h"
#include "RenderResourceNode.h"
#include "Helios/Renderer/Renderer.h"
#include "Helios/VirtualDevice/DeviceFrameBuffer.h"

namespace Helios
{
	RenderPassNode::RenderPassNode(const std::string& name, FrameGraph& frame_graph, const SharedPtr<IFrameGraphPass>& frame_graph_pass)
		: Node(name, frame_graph.GetDependencyGraph()), m_OwnerFrameGraph(frame_graph), m_OwnerFrameGraphPass(frame_graph_pass)
	{
	}

	RenderPassNode::~RenderPassNode()
	{
		Destroy();
	}

	/* 注册资源Handle */
	void RenderPassNode::RegisterResourceHandle(FrameGraphResourceHandle handle)
	{
		auto resource = m_OwnerFrameGraph.GetResource(handle);
		resource->NeedByPass(shared_from_this());
		m_RegisteredHandleIndices.insert(handle.GetIndex());
	}

	/* 判断资源Handle是否被注册 */
	bool RenderPassNode::IsResourceHandleRegistered(FrameGraphResourceHandle handle) const
	{
		return m_RegisteredHandleIndices.find(handle.GetIndex()) != m_RegisteredHandleIndices.cend();
	}

	/* 根据描述创建RenderPassData */
	uint32_t RenderPassNode::CreateRenderPassData(const std::string& name, const FrameGraphPassInfo::Descriptor& desc)
	{
		auto render_pass_data = CreateSharedPtr<RenderPassData>();
		render_pass_data->DebugName = name;
		render_pass_data->Descriptor = desc;
		auto& attachments = render_pass_data->Descriptor.Attachments;

		const auto& dependency_graph = m_OwnerFrameGraph.GetDependencyGraph();
		const auto incoming_connections = dependency_graph.GetIncomingConnectionsOfNode(shared_from_this());

		for (uint32_t i = 0; i < FrameGraphPassInfo::MAX_ATTACHMENT_NUM; ++i)
		{
			if (attachments.AttachmentArray[i])
			{
				render_pass_data->ValidAttachments[i] = attachments.AttachmentArray[i];

				const auto target_resource_handle = desc.Attachments.AttachmentArray[i];

				/* 判断资源节点是否为输入节点 */
				for (const auto& conn : incoming_connections)
				{
					auto resource_node = DynamicPtrCast<RenderResourceNode>(dependency_graph.GetNode(conn->FromNodeIdx));
					if (resource_node && (resource_node->GetResourceHandle() == target_resource_handle))
					{
						render_pass_data->IncomingResourceNodes[i] = resource_node;
						break;
					}
				}

				/* Attached 节点中，若非输入节点，即为输出节点 */
				render_pass_data->OutgoingResourceNodes[i] = m_OwnerFrameGraph.GetRenderResourceNode(target_resource_handle);
				if (render_pass_data->OutgoingResourceNodes[i] == render_pass_data->IncomingResourceNodes[i])
					render_pass_data->OutgoingResourceNodes[i] = nullptr;
			}
		}

		uint32_t idx = m_RenderPassDatas.size();
		m_RenderPassDatas.emplace_back(render_pass_data);
		return idx;
	}

	/* 获取RenderPassData */
	SharedPtr<RenderPassData> RenderPassNode::GetRenderPassData(uint32_t idx) const
	{
		if (idx >= m_RenderPassDatas.size())
			return nullptr;

		return m_RenderPassDatas[idx];
	}

	/* 构建阶段 */
	void RenderPassNode::Build()
	{
		for (const auto& render_pass_data : m_RenderPassDatas)
		{
			/* 生成资源数据 */
			for (uint32_t i = 0; i < FrameGraphPassInfo::MAX_ATTACHMENT_NUM; ++i)
			{
				if (render_pass_data->ValidAttachments[i])
					render_pass_data->RenderBufferUsage |= GetRenderBufferUsageByIndex(i);
			}


		}
	}

	/* 执行阶段 */
	void RenderPassNode::Execute(const FrameGraphResources& resources)
	{
		/* 生成当前节点的资源数据 */
		for (const auto& render_pass_data : m_RenderPassDatas)
		{
			/* 构造FrameBuffer描述 */
			FrameBufferDesc desc;
			desc.ViewportRegion = render_pass_data->Descriptor.ViewportRegion;
			desc.Samples = render_pass_data->Descriptor.Samples;
			desc.Usage = render_pass_data->RenderBufferUsage;
			desc.ColorClearValues = render_pass_data->Descriptor.ColorClearValues;

			/* 准备Color Attachments */
			desc.ColorRenderBuffers.reserve(MAX_COLOR_ATTACHMENT_NUM);
			for (uint32_t i = 0; i < MAX_COLOR_ATTACHMENT_NUM; ++i)
			{
				if (render_pass_data->ValidAttachments[i])
				{
					const auto& fg_texture = DynamicPtrCast<const Resource<FrameGraphTexture>>(m_OwnerFrameGraph.GetResource(render_pass_data->ValidAttachments[i]));
					desc.ColorRenderBuffers.push_back({ fg_texture->GetResource().Texture, fg_texture->GetSubDescriptor().Level, fg_texture->GetSubDescriptor().Layer });
				}
			}

			/* 准备Depth Attachment */
			if (render_pass_data->ValidAttachments[MAX_COLOR_ATTACHMENT_NUM])
			{
				const auto& fg_texture = DynamicPtrCast<const Resource<FrameGraphTexture>>(m_OwnerFrameGraph.GetResource(render_pass_data->ValidAttachments[MAX_COLOR_ATTACHMENT_NUM]));
				desc.DepthRenderBuffer.RenderTarget = fg_texture->GetResource().Texture;
				desc.DepthRenderBuffer.Level = fg_texture->GetSubDescriptor().Level;
				desc.DepthRenderBuffer.Layer = fg_texture->GetSubDescriptor().Layer;
			}

			/* 准备Stencil Attachment */
			if (render_pass_data->ValidAttachments[MAX_COLOR_ATTACHMENT_NUM + 1])
			{
				const auto& fg_texture = DynamicPtrCast<const Resource<FrameGraphTexture>>(m_OwnerFrameGraph.GetResource(render_pass_data->ValidAttachments[MAX_COLOR_ATTACHMENT_NUM + 1]));
				desc.StencilRenderBuffer.RenderTarget = fg_texture->GetResource().Texture;
				desc.StencilRenderBuffer.Level = fg_texture->GetSubDescriptor().Level;
				desc.StencilRenderBuffer.Layer = fg_texture->GetSubDescriptor().Layer;
			}

		/* 没有附件的 Pass（如 SideEffect Pass）直接渲染到默认 RT，不需要创建 FrameBuffer */
		if (render_pass_data->RenderBufferUsage == RenderBufferUsage::None)
		{
			render_pass_data->FrameBuffer.reset();
			continue;
		}

		/* 创建FrameBuffer */
		render_pass_data->FrameBuffer = DeviceFrameBuffer::Create(render_pass_data->DebugName + "_FrameBuffer", desc);
	}

	/* 对于没有附件的Pass，需要由后端绑定默认RenderTarget（如Metal的drawable），
	 * 否则Metal等显式RenderPass后端会因没有active render encoder而无法绘制。 */
	const bool need_default_render_pass = m_RenderPassDatas.empty() ||
		std::any_of(m_RenderPassDatas.begin(), m_RenderPassDatas.end(),
		[](const SharedPtr<RenderPassData>& data) { return data->RenderBufferUsage == RenderBufferUsage::None; });

	if (need_default_render_pass)
		Renderer::GetRenderAPI()->BeginDefaultRenderPass();

	/* 执行当前Pass */
	m_OwnerFrameGraphPass->Execute(resources);

	if (need_default_render_pass)
		Renderer::GetRenderAPI()->EndDefaultRenderPass();
}


	/* 销毁 */
	void RenderPassNode::Destroy()
	{
		/* 销毁当前节点的资源数据 */
		for (auto& render_pass_data : m_RenderPassDatas)
		{
			/* 销毁资源数据 */
			if (render_pass_data)
				render_pass_data->FrameBuffer.reset();
		}
		m_RenderPassDatas.clear();
	}

	/* 输出可视化文本 */
	std::string RenderPassNode::Graphvizify() const
	{
		std::string result_str;
		result_str.reserve(128);
		result_str.append("[label=\"");
		result_str.append(DebugName);
		result_str.append("\\nrefs: ");
		result_str.append(std::to_string(RefCount));
		result_str.append(", id: ");
		result_str.append(std::to_string(Index));
		for (auto& render_pass_data : m_RenderPassDatas)
		{
			if (render_pass_data)
			{
				result_str.append("\\ndata: ");
				result_str.append(render_pass_data->DebugName);
			}
		}
		result_str.append("\", style=filled, fillcolor=");
		result_str.append(IsValid() ? "darkorange" : "darkorange4");
		result_str.append("]");
		result_str.shrink_to_fit();

		return  result_str;
	}

	/* 指定当前节点发出的连线的可视化颜色 */
	std::string RenderPassNode::GraphvizifyConnectionColor() const
	{
		return "red";
	}
}
