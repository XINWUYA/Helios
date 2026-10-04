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
		/* 场景控制UI */
		ShowSceneControllerUI();
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

	/* 默认布局：左（层级树 / 资产浏览器）· 中（视口）· 右（属性 / 统计）。只在 ini 里没有这个
	 * DockSpace 节点、或者用户主动重置时才构建；resize 由 ImGui 按比例自适应。 */
	void MainEditorLayer::BuildDefaultLayout(ImGuiID dockspace_id)
	{
		PROFILE_FUNCTION();

		const ImGuiViewport* viewport = ImGui::GetMainViewport();

		ImGui::DockBuilderRemoveNode(dockspace_id);
		ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

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
		static bool show_style_editor = false;
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

			// Submit the DockSpace
			ImGuiIO& io = ImGui::GetIO();
			if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable)
			{
				const ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");

				/* 首次运行（ini 里没有该 DockSpace 节点）或用户点了 Reset Layout 时构建默认布局。
				 * DockBuilder 必须在 DockSpace() 之前调用；构建后不要再每帧重建，否则用户的自定义分栏会被覆盖。 */
				if (m_ResetLayoutRequested || ImGui::DockBuilderGetNode(dockspace_id) == nullptr)
				{
					m_ResetLayoutRequested = false;
					BuildDefaultLayout(dockspace_id);
				}

				ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
			}

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

				/* View：面板显隐 + 布局重置 */
				if (ImGui::BeginMenu("View"))
				{
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
					ImGui::MenuItem("Style Editor", nullptr, &show_style_editor);
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
		}
		ImGui::End();
		EditorTheme::PopDockHostBackground();

		/* Style Editor：实时调参，Export 后固化回 EditorTheme.h */
		if (show_style_editor)
			ImGui::ShowStyleEditor();
	}

	/* 显示场景控制UI */
	void MainEditorLayer::ShowSceneControllerUI()
	{
		PROFILE_FUNCTION();

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 2)); /* 指定间隔 */
		ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(0, 2));

		ImGui::Begin("##Scene Controller", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		{
			/* 图标尺寸取整数下限，避免窗口被压扁时 GetWindowHeight() 导致 size <= 0 */
			const float icon_size = std::max(16.0f, ImGui::GetWindowHeight() - 4.0f);
			const float panel_width = ImGui::GetWindowContentRegionMax().x;

			START_TRANSPARENT_BUTTON;

			constexpr float cursor_offset = 10.0f;
			/* 保存按钮 */
			ImGui::SetCursorPosX(cursor_offset);
			ImGuiExt::DrawCheckedImageButtonUI("Save", Icons::GetTexture(Icons::Id::Save), ImVec2(icon_size, icon_size), false,
				[&]()
				{
					m_Context.SaveScene();
				});

			/* 移动/旋转/缩放操作 */
			{
				/* translate */
				ImGui::SameLine(cursor_offset + icon_size * 2);
				const bool t_checked = m_Context.GetGizmoType() == ImGuizmo::OPERATION::TRANSLATE;
				ImGuiExt::DrawCheckedImageButtonUI("Translate", Icons::GetTexture(Icons::Id::Translate), ImVec2(icon_size, icon_size), t_checked,
					[&]()
					{
						m_Context.SetGizmoType(ImGuizmo::OPERATION::TRANSLATE);
					});

				/* rotate */
				ImGui::SameLine();
				const bool r_checked = m_Context.GetGizmoType() == ImGuizmo::OPERATION::ROTATE;
				ImGuiExt::DrawCheckedImageButtonUI("Rotate", Icons::GetTexture(Icons::Id::Rotate), ImVec2(icon_size, icon_size), r_checked,
					[&]()
					{
						m_Context.SetGizmoType(ImGuizmo::OPERATION::ROTATE);
					});

				/* scale */
				ImGui::SameLine();
				const bool s_checked = m_Context.GetGizmoType() == ImGuizmo::OPERATION::SCALE;
				ImGuiExt::DrawCheckedImageButtonUI("Scale", Icons::GetTexture(Icons::Id::Scale), ImVec2(icon_size, icon_size), s_checked,
					[&]()
					{
						m_Context.SetGizmoType(ImGuizmo::OPERATION::SCALE);
					});
			}

			/* 切换执行模式 */
			{
				ImGui::SameLine();
				const PlayMode play_mode = m_Context.GetPlayMode();
				const Icons::Id play_id = (play_mode == PlayMode::Edit) ? Icons::Id::Play : Icons::Id::Stop;
				ImGui::SetCursorPosX((panel_width - icon_size) * 0.5f);
				if (Icons::IconButton(play_id, ImVec2(icon_size, icon_size)))
				{
					m_Context.SetPlayMode((play_mode == PlayMode::Edit) ? PlayMode::Runtime : PlayMode::Edit);
				}
			}

			/* 配置 */
			{
				ImGui::SameLine(panel_width - cursor_offset - 20);
				START_STYLE_ALPHA(0.5f);
				if (Icons::IconButton(Icons::Id::Menu, ImVec2(20, 20)))
					ImGui::OpenPopup("ConfigPopup");
				END_STYLE_ALPHA;
			}

			END_TRANSPARENT_BUTTON;
		}

		ImGui::End();
		ImGui::PopStyleVar(2);
	}

	void MainEditorLayer::OnUpdate(float delta_time)
	{
		PROFILE_FUNCTION();
	}
}
