#pragma once
#include <imgui.h>

#include "EditorCommon.h"
#include "EditorContext.h"
#include "EditorResourceBrowser.h"
#include "LogPanel.h"

namespace Helios
{
	/* 编辑器壳层：菜单栏 / 场景控制条 / DockSpace 宿主 */
	class MainEditorLayer final : public ILayer
	{
	public:
		MainEditorLayer();
		~MainEditorLayer() override = default;

		void OnAttached() override;
		void OnDetached() override;
		void OnUpdate(float delta_time) override;
		void OnImGuiRender() override;
		void OnEvent(IEvent* event) override;

		/* 资源定位：层级面板的材质卡点了贴图 → 资源浏览器切目录并选中它。
		 * 跨面板通道由装配层（EditorApp）接线到 SceneEditorLayer。 */
		void RevealAsset(const std::string& relative_path) { m_ResourceBrowser.RevealAsset(relative_path); }

		/* 资源选中 → 属性面板（跨面板通道，反方向）：装配层把浏览器发布的选中项
		 * 接到 SceneEditorLayer 的属性面板上。 */
		void SetAssetSelectionSink(EditorResourceBrowser::AssetSelectionSink sink)
		{
			m_ResourceBrowser.SetAssetSelectionSink(std::move(sink));
		}

	private:
		/* 响应键盘 */
		bool OnKeyPressed(class KeyPressedEvent* event);
		/* 响应鼠标 */
		bool OnMouseButtonPressed(class MouseButtonPressedEvent* event);

		/* 显示菜单栏UI */
		void ShowMenuUI();
		/* 显示顶部工具栏（菜单栏与工作区之间的固定一行） */
		void ShowToolbarUI();

		/* 按 Panel::kDefaultLayout 用 DockBuilder 构建默认停靠布局。
		 * 必须在同一帧的 DockSpace() 之前调用（DockBuilder 的调用时序要求）；
		 * size 需与随后 DockSpace() 实际占用的区域一致。 */
		void BuildDefaultLayout(ImGuiID dockspace_id, const ImVec2& size);

		/* 跨面板通信 / 编辑状态收敛的唯一入口（消除按名字查 Layer + 强转） */
		EditorContext m_Context;
		/* 资源管理窗口 */
		EditorResourceBrowser m_ResourceBrowser;
		/* 日志窗口（Kernel / Editor 的运行时输出；数据源 = LogBuffer） */
		LogPanel m_LogPanel;
		/* 日志窗口是否显示（View 菜单 / 窗口右上角关闭按钮同源） */
		bool m_IsLogVisible{ true };
		/* 用户请求重置布局（View → Reset Layout），下一帧生效 */
		bool m_ResetLayoutRequested{ false };
		/* 实时样式编辑器（Options 菜单 / 工具栏选项开关） */
		bool m_ShowStyleEditor{ false };
	};
}
