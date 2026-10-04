#pragma once
#include "Helios/Core/Layer.h"
#include <memory>

namespace Helios
{
	class ImGuiRenderer;
	class FrameGraph;

	class ImGuiLayer : public ILayer
	{
	public:
		ImGuiLayer();
		~ImGuiLayer() override;

		virtual void OnAttached() override;
		virtual void OnDetached() override;
		virtual void OnImGuiRender() override;
		virtual void OnEvent(IEvent* event) override;

		/* 开始一帧内UI的收集 */
		void Begin();
		/* 完成UI构建，生成DrawData（供ImGuiPass在FrameGraph中渲染） */
		void End();

		void BlockEvents(bool block) { m_IsBlockEvents = block; }
		
		/* 获取渲染器 */
		ImGuiRenderer* GetRenderer() const { return m_Renderer.get(); }
		
	private:
		/* 多视口副窗口渲染（PlatformIO默认实现） */
		void RenderPlatformWindows();

		bool m_IsBlockEvents = false;
		std::unique_ptr<ImGuiRenderer> m_Renderer;
	};
}
