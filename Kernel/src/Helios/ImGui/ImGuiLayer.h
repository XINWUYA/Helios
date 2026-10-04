#pragma once
#include "Helios/Core/Layer.h"
#include <memory>
#include <string>

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

		/* 布局持久化开关：false 时令 ImGui 既不读也不写 ini（io.IniFilename = nullptr）。
		 * 用于窗口最小化/尺寸退化期间冻结布局保存，避免把钳制后的 DockNode 比例写盘。 */
		void SetLayoutSavingEnabled(bool enabled);

		/* 获取渲染器 */
		ImGuiRenderer* GetRenderer() const { return m_Renderer.get(); }
		
	private:
		/* 多视口副窗口渲染（PlatformIO默认实现） */
		void RenderPlatformWindows();

		bool m_IsBlockEvents = false;
		/* imgui.ini 的绝对路径（SetLayoutSavingEnabled 切换时复用） */
		std::string m_IniPath;
		std::unique_ptr<ImGuiRenderer> m_Renderer;
	};
}
