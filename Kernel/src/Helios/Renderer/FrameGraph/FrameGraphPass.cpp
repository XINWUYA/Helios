#include "Pch.h"
#include "FrameGraphPass.h"
#include "RenderPassNode.h"

namespace Helios
{
	void IFrameGraphPass::SetRenderPassNode(const SharedPtr<RenderPassNode>& node)
	{
		m_pRenderPassNode = node;
		m_DebugName = node != nullptr ? node->GetDebugName() : std::string();
	}
}
