#pragma once
#include "Helios/Core/Layer.h"
#include <functional>
#include <memory>
#include <string>

/* imgui 全局命名空间的类型（仅用于安装器签名，不引入 imgui 头） */
struct ImGuiIO;

namespace Helios
{
	class ImGuiRenderer;
	class FrameGraph;

	class ImGuiLayer : public ILayer
	{
	public:
		/* 界面字体 / 风格的安装器：应用注入自己的 UI 外观（字体、配色、间距等）。
		 * 在 ImGui 上下文建立后、字体图集上传前调用一次。
		 * 未注入 = ImGui 内置默认字体与默认风格 —— 引擎不预设编辑器外观。 */
		using StyleInstaller = std::function<void(ImGuiIO&, float content_scale)>;

		ImGuiLayer();
		~ImGuiLayer() override;

		void SetStyleInstaller(StyleInstaller installer) { m_StyleInstaller = std::move(installer); }

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
		/* 界面外观安装器（见 StyleInstaller；空 = ImGui 内置默认） */
		StyleInstaller m_StyleInstaller;
		std::unique_ptr<ImGuiRenderer> m_Renderer;
	};
}
