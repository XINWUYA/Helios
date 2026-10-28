#include "Pch.h"
#include "FrameGraphResources.h"
#include "FrameGraphResourceCache.h"
#include "Resource.h"
#include "RenderPassNode.h"
#include "Helios/VirtualDevice/DeviceTexture.h"

namespace Helios
{
	FrameGraphResources::FrameGraphResources(FrameGraph& frame_graph, RenderPassNode& render_pass_node)
		: m_FrameGraph(frame_graph), m_RenderPassNode(render_pass_node)
	{
	}

	/* 获取当前RenderPassNode名 */
	const std::string& FrameGraphResources::GetPassName() const
	{
		return m_RenderPassNode.GetDebugName();
	}

	/* 获取当前RenderPass的RenderTarget */
	SharedPtr<DeviceFrameBuffer> FrameGraphResources::GetPassRenderTarget(uint32_t idx) const
	{
		const auto& render_pass_data = m_RenderPassNode.GetRenderPassData(idx);
		if (!render_pass_data)
		{
			CORE_LOG_ERROR("Failed to access the RenderTarget of RenderPass({}) by index({}).", m_RenderPassNode.GetDebugName(), idx);
			return nullptr;
		}

		return render_pass_data->FrameBuffer;
	}

	/* 从FrameGraph中获取资源 */
	SharedPtr<IResource> FrameGraphResources::GetResource(FrameGraphResourceHandle handle) const
	{
		ASSERT(handle.IsInitialized(), "Handle is invalid.");
		auto resource = m_FrameGraph.GetResource(handle);

		if (!m_RenderPassNode.IsResourceHandleRegistered(handle))
		{
			CORE_LOG_ERROR("RenderPassNode({}) has not register the resource({}).", m_RenderPassNode.GetDebugName(), resource->GetName());
		}
		ASSERT(resource->GetRefCount(), "Resource is not needed by this RenderPassNode.");

		return resource;
	}

	/* 创建 */
	void FrameGraphTexture::Create(const std::string& name, const Descriptor& desc, Usage usage, FrameGraphResourceCache* cache)
	{
		PROFILE_FUNCTION();

		const TextureDesc texture_desc{ desc.Width, desc.Height, desc.Depth, desc.MipLevels, desc.Samples,
			desc.TextureFormat, desc.SamplerType, usage };

		/* 从资源池里取同描述的空闲纹理（前面 Pass 用完的、或上上帧留下的），
		 * 池子分配失败时退回直接分配 */
		if (cache)
		{
			Texture = cache->AcquireTexture(name, texture_desc);
			if (Texture)
				return;
		}

		Texture = DeviceTexture::Create(name, texture_desc);
	}

	/* 销毁 */
	void FrameGraphTexture::Destroy(const std::string& /*name*/, const Descriptor& desc, Usage usage, FrameGraphResourceCache* cache)
	{
		PROFILE_FUNCTION();

		if (cache && Texture)
		{
			const TextureDesc texture_desc{ desc.Width, desc.Height, desc.Depth, desc.MipLevels, desc.Samples,
				desc.TextureFormat, desc.SamplerType, usage };
			cache->ReleaseTexture(texture_desc, Texture);
		}

		Texture = nullptr;
	}
}
