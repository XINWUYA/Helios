#include "Pch.h"
#include "FrameGraphPass.h"
#include "RenderPassNode.h"
#include "Helios/Renderer/Renderer.h"

namespace Helios
{
	/* GPU 耗时作用域由渲染后端的 PushDebugGroup 一并驱动（同名同层级），
	 * 这里只负责配对压/弹组名，无需单独开作用域。 */
	void IFrameGraphPass::BeforeExecute()
	{
		const char* label = m_pRenderPassNode.lock()->GetDebugName().c_str();
		Renderer::GetRenderAPI()->PushDebugGroup(label);
	}

	void IFrameGraphPass::AfterExecute()
	{
		Renderer::GetRenderAPI()->PopDebugGroup();
	}
}
