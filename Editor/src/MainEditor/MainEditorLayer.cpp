#include "Pch.h"
#include "MainEditorLayer.h"
#include "EditorIcons.h"
#include "PanelRegistry.h"
#include "ImGuizmo.h"
#include "Helios/ImGui/EditorTheme.h"

namespace Helios
{
	MainEditorLayer::MainEditorLayer()
		: ILayer("MainEditorLayer")
	{
		PROFILE_FUNCTION();
	}

	void MainEditorLayer::OnAttached()
	{
		PROFILE_FUNCTION();

		/* 资源浏览器要跟同一条编辑历史打交道：它的文件操作（新建 / 重命名 / 删除）
		 * 要进这条历史，菜单与主工具栏的撤销 / 重做才管得着它们。
		 * 通道接到 EditorContext（Layer 的解析留在它内部），面板因此不认识 Layer / CommandStack。 */
		EditorResourceBrowser::HistorySink history;
		history.Execute = [this](UniquePtr<ICommand> command) { m_Context.Execute(std::move(command)); };
		m_ResourceBrowser.SetHistorySink(std::move(history));
	}

	void MainEditorLayer::OnDetached()
	{
		PROFILE_FUNCTION();

		ILayer::OnDetached();
	}

	void MainEditorLayer::OnImGuiRender()
	{
		PROFILE_FUNCTION();

		/* 菜单（含 DockSpace 宿主） */
		ShowMenuUI();
		/* 资源管理窗口 */
		m_ResourceBrowser.OnImGuiRenderer();
	}

	void MainEditorLayer::OnEvent(IEvent* event)
	{
		PROFILE_FUNCTION();

		if (!event)
			return;

		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<KeyPressedEvent>(BIND_EVENT_FUNC(MainEditorLayer::OnKeyPressed));
		dispatcher.Dispatch<MouseButtonPressedEvent>(BIND_EVENT_FUNC(MainEditorLayer::OnMouseButtonPressed));
	}

	bool MainEditorLayer::OnKeyPressed(KeyPressedEvent* event)
	{
		PROFILE_FUNCTION();

		if (event->GetRepeatCount() > 0)
			return false;

		const bool is_ctrl_pressed = Input::IsKeyPressed(Key::LeftControl) || Input::IsKeyPressed(Key::RightControl);
		const bool is_shift_pressed = Input::IsKeyPressed(Key::LeftShift) || Input::IsKeyPressed(Key::RightShift);
		const bool is_button_right_pressed = Input::IsMouseButtonPressed(Mouse::ButtonRight);

		switch (event->GetKeyCode())
		{
		case Key::N: /* Ctrl+N：新建场景 */
			if (is_ctrl_pressed)
				m_Context.NewScene();
			break;
		case Key::S: /* Ctrl+S: 保存场景; Ctrl+Shift+S: 另存为 */
			if (is_ctrl_pressed)
			{
				if (is_shift_pressed)
					m_Context.SaveSceneAs();
				else
					m_Context.SaveScene();
			}
			break;
		case Key::I: /* Ctrl+I：导入场景 */
			if (is_ctrl_pressed)
				m_Context.ImportScene();
			break;
		case Key::Z: /* Ctrl+Z：撤销；Ctrl+Shift+Z：重做 */
			if (is_ctrl_pressed)
			{
				if (is_shift_pressed)
					m_Context.Redo();
				else
					m_Context.Undo();
			}
			break;
		case Key::Y: /* Ctrl+Y：重做 */
			if (is_ctrl_pressed)
				m_Context.Redo();
			break;
		case Key::Q: /* Q/W/E/R：切换 Gizmo 操作 */
			if (!ImGuizmo::IsUsing() && !is_button_right_pressed)
				m_Context.SetGizmoType(-1);
			break;
		case Key::W:
			if (!ImGuizmo::IsUsing() && !is_button_right_pressed)
				m_Context.SetGizmoType(ImGuizmo::OPERATION::TRANSLATE);
			break;
		case Key::E:
			if (!ImGuizmo::IsUsing() && !is_button_right_pressed)
				m_Context.SetGizmoType(ImGuizmo::OPERATION::ROTATE);
			break;
		case Key::R:
			if (!ImGuizmo::IsUsing() && !is_button_right_pressed)
				m_Context.SetGizmoType(ImGuizmo::OPERATION::SCALE);
			break;
		default:
			break;
		}

		return true;
	}

	bool MainEditorLayer::OnMouseButtonPressed(MouseButtonPressedEvent* event)
	{
		PROFILE_FUNCTION();
		return false;
	}

	namespace
	{
		/* 工具条分组之间的竖向分隔线：占 1px 布局宽度并居中画出 */
		void ToolbarSeparator(float button_size, float gap)
		{
			ImGui::SameLine(0.0f, gap);
			ImGui::Dummy(ImVec2(1.0f, button_size));

			const ImVec2 min = ImGui::GetItemRectMin();
			const float x = min.x + 0.5f;
			const float center_y = min.y + button_size * 0.5f;
			const float half = button_size * 0.32f;

			ImGui::GetWindowDrawList()->AddLine(ImVec2(x, center_y - half), ImVec2(x, center_y + half),
				ImGui::GetColorU32(EditorTheme::Token::Border), 1.0f);

			ImGui::SameLine(0.0f, gap);
		}

		/* 撤销 / 重做的 tooltip：带上会撤销什么操作，与 Edit 菜单一致 */
		std::string MakeHistoryTooltip(const char* action, const char* label, const char* shortcut)
		{
			std::string text(action);
			if (label != nullptr)
			{
				text += " ";
				text += label;
			}
			text += "  (";
			text += shortcut;
			text += ")";
			return text;
		}
	}

	/* 默认布局：左（层级树 / 资产浏览器）· 中（视口）· 右（属性 / 统计）。只在 ini 里没有这个
	 * DockSpace 节点、或者用户主动重置时才构建；resize 由 ImGui 按比例自适应。 */
	void MainEditorLayer::BuildDefaultLayout(ImGuiID dockspace_id, const ImVec2& size)
	{
		PROFILE_FUNCTION();

		ImGui::DockBuilderRemoveNode(dockspace_id);
		ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodeSize(dockspace_id, size);

		ImGuiID center = dockspace_id;
		ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);
		ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, nullptr, &center);
		ImGuiID left_bottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.40f, nullptr, &left);
		ImGuiID right_bottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.40f, nullptr, &right);

		/* 语义槽位 -> 实际 DockNode */
		const auto slot_to_node = [&](Panel::DockSlot slot) -> ImGuiID
		{
			switch (slot)
			{
			case Panel::DockSlot::Center:      return center;
			case Panel::DockSlot::LeftTop:     return left;
			case Panel::DockSlot::LeftBottom:  return left_bottom;
			case Panel::DockSlot::RightTop:    return right;
			case Panel::DockSlot::RightBottom: return right_bottom;
			default:                           return center;
			}
		};

		for (const auto& desc : Panel::kDefaultLayout)
			ImGui::DockBuilderDockWindow(desc.Id, slot_to_node(desc.Slot));

		ImGui::DockBuilderFinish(dockspace_id);
	}

	void MainEditorLayer::ShowMenuUI()
	{
		PROFILE_FUNCTION();

		static bool p_open = true;
		static bool opt_fullscreen = true;
		static bool opt_padding = false;
		static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_None;

		// We are using the ImGuiWindowFlags_NoDocking flag to make the parent window not dockable into,
		// because it would be confusing to have two docking targets within each others.
		ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
		if (opt_fullscreen)
		{
			const ImGuiViewport* viewport = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(viewport->WorkPos);
			ImGui::SetNextWindowSize(viewport->WorkSize);
			ImGui::SetNextWindowViewport(viewport->ID);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
			window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
			window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
		}
		else
		{
			dockspace_flags &= ~ImGuiDockNodeFlags_PassthruCentralNode;
		}

		// When using ImGuiDockNodeFlags_PassthruCentralNode, DockSpace() will render our background
		// and handle the pass-thru hole, so we ask Begin() to not render a background.
		if (dockspace_flags & ImGuiDockNodeFlags_PassthruCentralNode)
			window_flags |= ImGuiWindowFlags_NoBackground;

		// Important: note that we proceed even if Begin() returns false (aka window is collapsed).
		// This is because we want to keep our DockSpace() active. If a DockSpace() is inactive,
		// all active windows docked into it will lose their parent and become undocked.
		// We cannot preserve the docking relationship between an active window and an inactive docking, otherwise
		// any change of dockspace/settings would lead to windows being stuck in limbo and never being visible.
		/* 宿主画布背景压暗一档（Neutral0）：停靠面板（Neutral2）形成"浮在画布上"的层级 */
		EditorTheme::PushDockHostBackground();
		if (!opt_padding)
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		/* ### 之后为窗口 ID：保持不变，旧的 imgui.ini 停靠布局继续有效 */
		ImGui::Begin("Helios###DockSpace Demo", &p_open, window_flags);
		{
			if (!opt_padding)
				ImGui::PopStyleVar();

			if (opt_fullscreen)
				ImGui::PopStyleVar(2);

			/* 菜单栏 */
			if (ImGui::BeginMenuBar())
			{
				if (ImGui::BeginMenu("File"))
				{
					if (ImGui::MenuItem("New Scene", "Ctrl+N"))
						m_Context.NewScene();

					if (ImGui::MenuItem("Open Scene", "Ctrl+O"))
						m_Context.ImportScene();

					if (ImGui::MenuItem("Save Scene", "Ctrl+S"))
						m_Context.SaveScene();

					if (ImGui::MenuItem("Save Scene As", "Ctrl+Shift+S"))
						m_Context.SaveSceneAs();

					if (ImGui::MenuItem("Import Model(.obj)", "Ctrl+I"))
					{
						m_Context.SetModelEditorActive(true);
						m_Context.ImportModel();
					}

					if (ImGui::MenuItem("Export Mesh & Mtl(.mesh & .mtl)", "Ctrl+E"))
					{
						/* TODO: 尚未实现导出 */
					}

					if (ImGui::MenuItem("Exit"))
						Application::Instance()->Close();

					ImGui::EndMenu();
				}

				/* Edit：编辑历史 */
				if (ImGui::BeginMenu("Edit"))
				{
					const char* undo_label = m_Context.GetUndoLabel();
					const char* redo_label = m_Context.GetRedoLabel();

					/* 菜单里带上操作名，用户能看到 Ctrl+Z 会撤销什么 */
					const std::string undo_text = "Undo" + (undo_label != nullptr ? std::string(" ") + undo_label : std::string());
					const std::string redo_text = "Redo" + (redo_label != nullptr ? std::string(" ") + redo_label : std::string());

					if (ImGui::MenuItem(undo_text.c_str(), "Ctrl+Z", false, m_Context.CanUndo()))
						m_Context.Undo();

					if (ImGui::MenuItem(redo_text.c_str(), "Ctrl+Y", false, m_Context.CanRedo()))
						m_Context.Redo();

					ImGui::EndMenu();
				}

				/* View：面板显隐 + 布局重置。
				 * 分两级：视口窗口（窗口右上角的关闭按钮只能靠这里恢复）与面板整层启用；
				 * 场景的新建 / 打开也会自动唤回视口窗口，避免「关掉后再也打不开」。 */
				if (ImGui::BeginMenu("View"))
				{
					bool scene_viewport_visible = m_Context.IsSceneViewportVisible();
					if (ImGui::MenuItem("Scene Viewport", nullptr, &scene_viewport_visible))
						m_Context.SetSceneViewportVisible(scene_viewport_visible);

					bool model_viewport_visible = m_Context.IsModelViewportVisible();
					if (ImGui::MenuItem("Model Viewport", nullptr, &model_viewport_visible))
						m_Context.SetModelViewportVisible(model_viewport_visible);

					ImGui::Separator();

					bool scene_active = m_Context.IsSceneEditorActive();
					if (ImGui::MenuItem("Scene Editor", nullptr, &scene_active))
						m_Context.SetSceneEditorActive(scene_active);

					bool model_active = m_Context.IsModelEditorActive();
					if (ImGui::MenuItem("Model Editor", nullptr, &model_active))
						m_Context.SetModelEditorActive(model_active);

					ImGui::Separator();

					if (ImGui::MenuItem("Reset Layout"))
						m_ResetLayoutRequested = true;

					ImGui::EndMenu();
				}

				if (ImGui::BeginMenu("Options"))
				{
					// Disabling fullscreen would allow the window to be moved to the front of other windows,
					// which we can't undo at the moment without finer window depth/z control.
					ImGui::MenuItem("Fullscreen", NULL, &opt_fullscreen);
					ImGui::MenuItem("Padding", NULL, &opt_padding);
					ImGui::MenuItem("Style Editor", nullptr, &m_ShowStyleEditor);
					ImGui::Separator();

					if (ImGui::MenuItem("Flag: NoSplit", "", (dockspace_flags & ImGuiDockNodeFlags_NoSplit) != 0)) { dockspace_flags ^= ImGuiDockNodeFlags_NoSplit; }
					if (ImGui::MenuItem("Flag: NoResize", "", (dockspace_flags & ImGuiDockNodeFlags_NoResize) != 0)) { dockspace_flags ^= ImGuiDockNodeFlags_NoResize; }
					if (ImGui::MenuItem("Flag: NoDockingInCentralNode", "", (dockspace_flags & ImGuiDockNodeFlags_NoDockingInCentralNode) != 0)) { dockspace_flags ^= ImGuiDockNodeFlags_NoDockingInCentralNode; }
					if (ImGui::MenuItem("Flag: AutoHideTabBar", "", (dockspace_flags & ImGuiDockNodeFlags_AutoHideTabBar) != 0)) { dockspace_flags ^= ImGuiDockNodeFlags_AutoHideTabBar; }
					if (ImGui::MenuItem("Flag: PassthruCentralNode", "", (dockspace_flags & ImGuiDockNodeFlags_PassthruCentralNode) != 0, opt_fullscreen)) { dockspace_flags ^= ImGuiDockNodeFlags_PassthruCentralNode; }
					ImGui::Separator();

					if (ImGui::MenuItem("Close", NULL, false))
						p_open = false;
					ImGui::EndMenu();
				}

				ImGui::EndMenuBar();
			}

			/* 顶部工具栏：菜单栏与工作区之间的固定一行（不参与停靠） */
			ShowToolbarUI();

			/* DockSpace 占满工具条之后的剩余区域 */
			ImGuiIO& io = ImGui::GetIO();
			if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable)
			{
				const ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
				const ImVec2 dockspace_size = ImGui::GetContentRegionAvail();

				/* 首次运行（ini 里没有该 DockSpace 节点）或用户点了 Reset Layout 时构建默认布局。
				 * DockBuilder 必须在 DockSpace() 之前调用；构建后不要再每帧重建，否则用户的自定义分栏会被覆盖。 */
				if (m_ResetLayoutRequested || ImGui::DockBuilderGetNode(dockspace_id) == nullptr)
				{
					m_ResetLayoutRequested = false;
					BuildDefaultLayout(dockspace_id, dockspace_size);
				}

				ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
			}
		}
		ImGui::End();
		EditorTheme::PopDockHostBackground();

		/* Style Editor：实时调参，Export 后固化回 EditorTheme.h */
		if (m_ShowStyleEditor)
			ImGui::ShowStyleEditor();
	}

	/* 顶部工具栏：固定一行、横跨整个窗口宽，跟菜单栏一起构成顶部 chrome。分组从左到右按操作
	 * 频率排：文件 → 编辑历史 → 变换；运行控制单独贴右端，免得误点。 */
	void MainEditorLayer::ShowToolbarUI()
	{
		PROFILE_FUNCTION();

		constexpr float kToolbarHeight = 34.0f;
		constexpr float kButtonSize = 24.0f;
		constexpr float kGroupGap = 9.0f;
		constexpr float kSidePadding = 8.0f;

		ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Token::Neutral3);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 0.0f));

		ImGui::BeginChild("##MainToolbar", ImVec2(0.0f, kToolbarHeight), false,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavFocus);
		{
			const ImVec2 button_size(kButtonSize, kButtonSize);

			/* 子窗口自身没有内边距（除非显式要求 AlwaysUseWindowPadding），
			 * 位置必须显式指定，否则按钮会贴着顶边。 */
			ImGui::SetCursorPos(ImVec2(kSidePadding, (kToolbarHeight - kButtonSize) * 0.5f));

			/* ---- 文件 ---- */
			if (Icons::IconButton(Icons::Id::NewScene, button_size, false, "New Scene  (Ctrl+N)"))
				m_Context.NewScene();

			ImGui::SameLine();
			if (Icons::IconButton(Icons::Id::OpenScene, button_size, false, "Open Scene  (Ctrl+O)"))
				m_Context.ImportScene();

			ImGui::SameLine();
			if (Icons::IconButton(Icons::Id::Save, button_size, false, "Save Scene  (Ctrl+S)"))
				m_Context.SaveScene();

			/* ---- 编辑历史 ---- */
			ToolbarSeparator(kButtonSize, kGroupGap);

			const std::string undo_tip = MakeHistoryTooltip("Undo", m_Context.GetUndoLabel(), "Ctrl+Z");
			const std::string redo_tip = MakeHistoryTooltip("Redo", m_Context.GetRedoLabel(), "Ctrl+Y");

			ImGui::BeginDisabled(!m_Context.CanUndo());
			if (Icons::IconButton(Icons::Id::Undo, button_size, false, undo_tip.c_str()))
				m_Context.Undo();
			ImGui::EndDisabled();

			ImGui::SameLine();
			ImGui::BeginDisabled(!m_Context.CanRedo());
			if (Icons::IconButton(Icons::Id::Redo, button_size, false, redo_tip.c_str()))
				m_Context.Redo();
			ImGui::EndDisabled();

			/* ---- 变换 ---- */
			ToolbarSeparator(kButtonSize, kGroupGap);

			const int gizmo_type = m_Context.GetGizmoType();
			if (Icons::IconButton(Icons::Id::Translate, button_size, gizmo_type == ImGuizmo::OPERATION::TRANSLATE, "Translate  (W)"))
				m_Context.SetGizmoType(ImGuizmo::OPERATION::TRANSLATE);

			ImGui::SameLine();
			if (Icons::IconButton(Icons::Id::Rotate, button_size, gizmo_type == ImGuizmo::OPERATION::ROTATE, "Rotate  (E)"))
				m_Context.SetGizmoType(ImGuizmo::OPERATION::ROTATE);

			ImGui::SameLine();
			if (Icons::IconButton(Icons::Id::Scale, button_size, gizmo_type == ImGuizmo::OPERATION::SCALE, "Scale  (R)"))
				m_Context.SetGizmoType(ImGuizmo::OPERATION::SCALE);

			/* ---- 运行控制：水平居中 ---- */
			const float previous_right = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
			const float centered_x = ImGui::GetWindowWidth() * 0.5f - kButtonSize * 0.5f;
			ImGui::SameLine(std::max(centered_x, previous_right + kGroupGap));

			const bool running = (m_Context.GetPlayMode() == PlayMode::Runtime);
			if (Icons::IconButton(running ? Icons::Id::Stop : Icons::Id::Play, button_size, running,
				running ? "Stop" : "Play"))
			{
				m_Context.SetPlayMode(running ? PlayMode::Edit : PlayMode::Runtime);
			}
		}
		ImGui::EndChild();

		ImGui::PopStyleVar();
		ImGui::PopStyleColor();

		/* 与下方工作区之间的分隔线 */
		const ImVec2 bar_min = ImGui::GetItemRectMin();
		const ImVec2 bar_max = ImGui::GetItemRectMax();
		ImGui::GetWindowDrawList()->AddLine(ImVec2(bar_min.x, bar_max.y), ImVec2(bar_max.x, bar_max.y),
			ImGui::GetColorU32(EditorTheme::Token::Separator), 1.0f);
	}

	void MainEditorLayer::OnUpdate(float delta_time)
	{
		PROFILE_FUNCTION();
	}
}
