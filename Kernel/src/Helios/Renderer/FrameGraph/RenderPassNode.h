#pragma once
#include <unordered_set>
#include <glm/glm.hpp>
#include "FrameGraph.h"
#include "FrameGraphResourceHandle.h"
#include "FrameGraphResources.h"

namespace Helios
{
	class IFrameGraphPass;
	class RenderResourceNode;
	class DeviceFrameBuffer;

	/* 包含RenderPass所需的数据，一个RenderPass可能包含多组数据 */
	struct RenderPassData
	{
		std::string DebugName{};																						/* 数据名 */
		FrameGraphPassInfo::Descriptor Descriptor{};																	/* 描述 */
		SharedPtr<RenderResourceNode> IncomingResourceNodes[FrameGraphPassInfo::MAX_ATTACHMENT_NUM] = {};				/* 输入的资源节点 */
		SharedPtr<RenderResourceNode> OutgoingResourceNodes[FrameGraphPassInfo::MAX_ATTACHMENT_NUM] = {};				/* 输出的资源节点 */
		FrameGraphResourceHandleTyped<FrameGraphTexture> ValidAttachments[FrameGraphPassInfo::MAX_ATTACHMENT_NUM] = {}; /* 收集有效的Attachments */
		RenderBufferUsage RenderBufferUsage{ RenderBufferUsage::None };													/* RenderBuffer的使用情况 */
		SharedPtr<DeviceFrameBuffer> FrameBuffer{ nullptr };																	/* FrameBuffer */
	};

	/* RenderPassNode类，作为FrameGraph依赖关系图中的一个节点 */
	class RenderPassNode : public DependencyGraph::Node, public std::enable_shared_from_this<RenderPassNode>
	{
	public:
		RenderPassNode(const std::string& name, FrameGraph& frame_graph, const SharedPtr<IFrameGraphPass>& frame_graph_pass);
		virtual ~RenderPassNode() override;

		/* 获取Pass名 */
		const std::string& GetDebugName() const { return DebugName; }

		/* 计算 Pass：只派发、不画附件（结果写在图外的缓冲里）。
		 * 执行时不绑默认渲染目标，抓取时也不拿默认目标快照当输出。 */
		bool IsComputePass{ false };

		/* 对当前RenderPass, 在执行之前，需要创建需要的资源，对之后的RenderPass不会再使用的资源，应及时销毁。 */
		std::vector<SharedPtr<IResource>>& GetResourcesNeedPrepared() { return m_ResourcesNeedPrepared; }
		const std::vector<SharedPtr<IResource>>& GetResourcesNeedPrepared() const { return m_ResourcesNeedPrepared; }
		std::vector<SharedPtr<IResource>>& GetResourcesNeedDestroy() { return m_ResourcesNeedDestroy; }
		const std::vector<SharedPtr<IResource>>& GetResourcesNeedDestroy() const { return m_ResourcesNeedDestroy; }

		/* 注册资源Handle */
		void RegisterResourceHandle(FrameGraphResourceHandle handle);
		/* 判断资源Handle是否被注册 */
		[[nodiscard]] bool IsResourceHandleRegistered(FrameGraphResourceHandle handle) const;

		/* 根据描述创建RenderPassData */
		uint32_t CreateRenderPassData(const std::string& name, const FrameGraphPassInfo::Descriptor& desc);
		/* 获取RenderPassData */
		[[nodiscard]] SharedPtr<RenderPassData> GetRenderPassData(uint32_t idx = 0) const;

		/* 构建阶段 */
		void Build();
		/* 执行阶段 */
		void Execute(const FrameGraphResources& resources);
		/* 销毁 */
		void Destroy();

	private:
		/* 输出可视化设置 */
		[[nodiscard]] std::string Graphvizify() const override;
		[[nodiscard]] std::string GraphvizifyConnectionColor() const override;

		/* 所属FrameGraph */
		FrameGraph& m_OwnerFrameGraph;
		/* 所属的FrameGraphPass */
		SharedPtr<IFrameGraphPass> m_OwnerFrameGraphPass{ nullptr };
		/* 已注册的资源Handles */
		std::unordered_set<uint16_t> m_RegisteredHandleIndices{};
		/* 需要准备的资源，仅持有指针，内存由FrameGraph统一管理 */
		std::vector<SharedPtr<IResource>> m_ResourcesNeedPrepared{};
		/* 需要释放的资源，仅持有指针，内存由FrameGraph统一管理 */
		std::vector<SharedPtr<IResource>> m_ResourcesNeedDestroy{};
		/*当前Pass数据 */
		std::vector<SharedPtr<RenderPassData>> m_RenderPassDatas{};
	};
}
