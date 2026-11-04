#include "Pch.h"
#include "MainEditorLayer.h"
#include "UI/DebugViewMenu.h"
#include "UI/ImGuiExtensions.h"
#include "Panels/PanelWindow.h"
#include "Viewport/EditorBuiltinCamera.h"
#include <cstdio>
#include "Viewport/SceneGizmos.h"
#include "UI/EditorIcons.h"
#include "UI/PanelChrome.h"
#include "UI/PanelRegistry.h"
#include "Common/FileDialog.h"
#include "Common/EditorLayouts.h"
#include "Command/AssetFileOps.h"
#include "Helios/Common/PathUtils.h"
#include "Helios/Common/AssetPath.h"
#include "Helios/Core/AssetManager.h"
#include "UI/EditorTheme.h"
#include "Helios/Renderer/Renderer.h"
#include <fstream>

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

		/* 双击资源浏览器里的 .scn → 打开场景：同样经 EditorContext 转达（面板不认识场景层）。
		 * 路径按绝对路径给 —— 与导入对话框 / 拖入同一口径。 */
		EditorResourceBrowser::SceneOpenSink scene_open;
		scene_open.Open = [this](const std::string& path) { m_Context.OpenScene(path); };
		m_ResourceBrowser.SetSceneOpenSink(std::move(scene_open));

		/* 双击 .anim → 时间轴独立编辑（不需要先找一个挂着这个片段的实体） */
		EditorResourceBrowser::AnimationClipOpenSink clip_open;
		clip_open.Open = [this](const std::string& path) { m_Context.OpenAnimationClip(path); };
		m_ResourceBrowser.SetAnimationClipOpenSink(std::move(clip_open));

		/* 项目本地设置先读：脚本开关要赶在首次编译 / 加载之前生效 */
		m_Settings = EditorSettings::Load();
		EditorSettings::Apply(m_Settings);
		m_ScriptModule.SetAutoReloadEnabled(m_Settings.AutoReloadScripts);
		m_ScriptModule.SetAutoCompileEnabled(m_Settings.AutoCompileScripts);
		m_ShaderReloader.SetAutoReloadEnabled(m_Settings.AutoReloadShaders);

		/* 启动时把已编译好的脚本模块挂上：类注册进 ScriptRegistry，属性面板与场景加载才有类可用 */
		m_ScriptModule.LoadLatest();
	}

	void MainEditorLayer::OnDetached()
	{
		PROFILE_FUNCTION();

		ILayer::OnDetached();
	}

	void MainEditorLayer::OnImGuiRender()
	{
		PROFILE_FUNCTION();

		/* 还原上次关闭时的停靠前台页签：须在任何窗口提交之前（节点回写读当前焦点窗口） */
		m_LayoutRestore.Update();

		/* 时间轴里方向键用于微调：鼠标停在时刻区时，把这一帧的导航移动请求在提交任何控件之前吃掉，
		 * 方向键就只做微调、不会把焦点挪到别的面板；其它位置方向键和 Tab 仍然归导航 */
		if (m_Context.WantsTimelineArrowKeys()
			&& (KeyDown(ImGuiKey_LeftArrow) || KeyDown(ImGuiKey_RightArrow)))
		{
			/* 只吃方向键的移动请求：Tab 的请求（NavMoveDir == None）原样保留 */
			ImGuiContext& context = *ImGui::GetCurrentContext();
			if (context.NavMoveDir != ImGuiDir_None)
			{
				context.NavMoveScoringItems = false;
				context.NavMoveSubmitted = false;
				context.NavMoveForwardToNextFrame = false;
				context.NavMoveDir = ImGuiDir_None;
				context.NavMoveFlags = ImGuiNavMoveFlags_None;
			}
		}

		/* 菜单（含 DockSpace 宿主） */
		ShowMenuUI();
		/* 资源管理窗口 */
		m_ResourceBrowser.OnImGuiRenderer();
		/* 日志窗口（Kernel / Editor 的运行时输出） */
		m_LogPanel.OnImGuiRender(Panel::VisibleFlag(Panel::kLog));
		ShowEditorCameraUI();
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

		/* ImGui 正收键盘（输入框激活等）时快捷键让位（键进编辑框，不进命令） */
		if (ImGui::GetIO().WantCaptureKeyboard)
			return true;

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
			if (!m_Context.IsGizmoDragging() && !is_button_right_pressed)
				m_Context.SetGizmoType(-1);
			break;
		case Key::W:
			if (!m_Context.IsGizmoDragging() && !is_button_right_pressed)
				m_Context.SetGizmoType(0);
			break;
		case Key::E:
			if (!m_Context.IsGizmoDragging() && !is_button_right_pressed)
				m_Context.SetGizmoType(1);
			break;
		case Key::R:
			if (!m_Context.IsGizmoDragging() && !is_button_right_pressed)
				m_Context.SetGizmoType(2);
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
			ImGui::Dummy(ImVec2(Scaled(1.0f), button_size));

			const ImVec2 min = ImGui::GetItemRectMin();
			const float x = min.x + 0.5f;
			const float center_y = min.y + button_size * 0.5f;
			const float half = button_size * 0.32f;

			ImGui::GetWindowDrawList()->AddLine(ImVec2(x, center_y - half), ImVec2(x, center_y + half),
				ImGui::GetColorU32(EditorTheme::Token::Border), Scaled(1.0f));

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

		/* ---- 调试视图（Debug View）：档位表与行版式在 DebugViewMenu.h（场景视口与模型
		 * 编辑器预览共用同一份）；这里只把点选落到编辑器上下文 ---- */
		void DrawDebugViewMenuItems(EditorContext& context)
		{
			DebugViewMenu::DrawItems(context.GetDebugViewMode(),
				[&context](DebugViewMode mode) { context.SetDebugViewMode(mode); });
		}

		/* 当前档位的显示名（工具栏 tooltip 用；Shaded / 未知档返回空） */
		const char* DebugViewModeLabel(DebugViewMode mode)
		{
			return DebugViewMenu::ModeLabel(mode);
		}

		/* 菜单栏下拉的统一版式：圆角 / 内边距 / 行距和其它浮层菜单同一套。
		 * 要压在 BeginMenu 之前 —— 弹层的窗口参数在本帧第一次 Begin 时就锁定了。 */
		bool BeginBarMenu(const char* label)
		{
			PanelChrome::PushMenuStyle(true);
			if (ImGui::BeginMenu(label))
				return true;

			PanelChrome::PopMenuStyle();
			return false;
		}

		void EndBarMenu()
		{
			ImGui::EndMenu();
			PanelChrome::PopMenuStyle();
		}

		bool BarMenuItem(const char* label, const char* shortcut = nullptr, bool enabled = true)
		{
			return PanelChrome::MenuItemWithIcon(Icons::Id::None, label, shortcut, enabled,
				ImGui::GetFrameHeight());
		}

		bool BeginBarSubmenu(const char* label)
		{
			return PanelChrome::BeginMenuWithIcon(Icons::Id::None, label,
				ImGui::GetFrameHeight());
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
		/* 中栏再切出底部一条（日志窗口）：只剖中栏，左右两栏的纵向划分不变 */
		ImGuiID center_bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr, &center);

		/* 语义槽位 -> 实际 DockNode */
		const auto slot_to_node = [&](Panel::DockSlot slot) -> ImGuiID
		{
			switch (slot)
			{
			case Panel::DockSlot::Center:       return center;
			case Panel::DockSlot::LeftTop:      return left;
			case Panel::DockSlot::LeftBottom:   return left_bottom;
			case Panel::DockSlot::RightTop:     return right;
			case Panel::DockSlot::RightBottom:  return right_bottom;
			case Panel::DockSlot::CenterBottom: return center_bottom;
			default:                            return center;
			}
		};

		for (const auto& desc : Panel::kPanels)
		{
			if (desc.Slot != Panel::DockSlot::Floating)
				ImGui::DockBuilderDockWindow(desc.Id, slot_to_node(desc.Slot));
		}

		ImGui::DockBuilderFinish(dockspace_id);
	}

	void MainEditorLayer::CreateScriptTemplate()
	{
		const std::string path = FileDialog::SaveFile("script(*.cpp)\0*.cpp\0", "NewScript.cpp");
		if (path.empty())
			return;

		/* 脚本必须放在当前项目的 Scripts 下：CMake 只从那里收集源文件 */
		std::error_code error;
		const std::filesystem::path target = std::filesystem::weakly_canonical(PathFromUtf8(path), error);
		const std::filesystem::path script_root =
			std::filesystem::weakly_canonical(g_ProjectPath / "Scripts", error);
		if (target.parent_path() != script_root)
		{
			CORE_LOG_ERROR("[Script] Scripts must be saved under {}.", PathToUtf8(script_root));
			return;
		}

		/* 模板与资源浏览器「新建 > Script」是同一份，类名跟着文件名走 */
		const std::string content = AssetFileTemplate(".cpp", PathToUtf8(target.stem()));

		std::ofstream file(target, std::ios::binary | std::ios::trunc);
		if (!file.is_open())
		{
			CORE_LOG_ERROR("[Script] Failed to write {}.", path);
			return;
		}

		file << content;
		file.close();

		CORE_LOG_INFO("[Script] Created {}. Pick Build > Recompile Scripts to load it.", path);
	}

	void MainEditorLayer::ShowMenuUI()
	{
		PROFILE_FUNCTION();

		/* 宿主窗口铺满主视口、不参与停靠：菜单栏与工具栏都挂在它下面 */
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(viewport->WorkPos);
		ImGui::SetNextWindowSize(viewport->WorkSize);
		ImGui::SetNextWindowViewport(viewport->ID);

		const ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking
			| ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize
			| ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

		/* 宿主画布背景压暗一档（Neutral0）：停靠面板（Neutral2）形成"浮在画布上"的层级 */
		EditorTheme::PushDockHostBackground();
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		/* ### 之后为窗口 ID：保持不变，旧的 imgui.ini 停靠布局继续有效。
		 * 菜单栏高度 = 字体高 + 2×主题的 FramePadding.y（24px），左端的徽标正好嵌进去 */
		ImGui::Begin("Helios###DockSpace Demo", nullptr, window_flags);
		{
			ImGui::PopStyleVar(3);

			/* 菜单栏 */
			if (ImGui::BeginMenuBar())
			{
				/* 菜单栏底色：强调色自左向右淡出。画在这里是因为宿主窗口的绘制裁剪区不含菜单栏，
				 * 只有 BeginMenuBar 把裁剪框开到菜单栏内；MenuBarBg 仍是底，菜单项后画压在上面 */
				const ImRect bar = ImGui::GetCurrentWindow()->MenuBarRect();
				const ImVec2 fade_end(ImMin(bar.Max.x, bar.Min.x + Scaled(560.0f)), bar.Max.y);
				const ImVec4 accent = EditorTheme::Token::Accent;
				const ImU32 tint = ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, 0.22f));
				const ImU32 fade = ImGui::GetColorU32(ImVec4(accent.x, accent.y, accent.z, 0.0f));
				ImGui::GetWindowDrawList()->AddRectFilledMultiColor(bar.Min, fade_end, tint, fade, fade, tint);

				/* 左端留一档白边：徽标不贴死在窗口边缘上 */
				ImGui::Indent(Scaled(7.0f));

				/* 品牌记号：太阳徽标，与启动页同一张贴图、按强调色着色。没渲染设备时（离屏预览）
				 * 不碰资产缓存，菜单栏就空着不占位 */
				if (!m_EmblemRequested)
				{
					m_EmblemRequested = true;
					if (Renderer::GetRenderAPI() != nullptr)
						m_pEmblem = AssetManager::Instance().GetOrLoad<DeviceTexture>(
							PathToUtf8(g_EngineResPath / "EditorRes" / "branding" / "sun-emblem.png"));
				}
				if (m_pEmblem != nullptr && m_pEmblem->IsLoaded())
				{
					/* 与菜单项同中线：项框高 = FrameHeight，文字在框内居中，不是贴着文字行 */
					const float row = ImGui::GetFrameHeight();
					const float mark = row - Scaled(3.0f);
					/* 缩到 20pt 后线稿大半像素落在抗锯齿边缘，直接吃 Accent 会比色板暗一截，
					 * 往白里提一档才和菜单文字的亮度站平 */
					const ImVec4 mark_color = EditorTheme::MixRgb(accent,
						ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.40f);
					const ImVec2 cursor = ImGui::GetCursorScreenPos();
					const ImVec2 mark_min(cursor.x, cursor.y + (row - mark) * 0.5f);
					ImGui::GetWindowDrawList()->AddImage((ImTextureID)m_pEmblem.get(),
						mark_min, ImVec2(mark_min.x + mark, mark_min.y + mark),
						ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), ImGui::GetColorU32(mark_color));
					ImGui::Dummy(ImVec2(mark, row));
				}

				/* 顶级项展开时用淡强调底：列表选中那档 40% 放到这条窄栏里太重 */
				ImGui::PushStyleColor(ImGuiCol_Header, EditorTheme::WithAlpha(accent, 0.28f));

				ShowFileMenu();
				ShowWindowMenu();
				ShowRenderingMenu();
				ShowScriptsMenu();
				ShowShadersMenu();

				ImGui::PopStyleColor();
				ImGui::Unindent(Scaled(7.0f));

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

				ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
			}

			/* 保存布局：菜单点完就收起，命名框在这里弹（模态要留在宿主窗口的作用域里） */
			if (m_ShowSaveLayoutPopup)
			{
				m_ShowSaveLayoutPopup = false;
				m_LayoutName[0] = '\0';
				ImGui::OpenPopup("Save Layout");
			}

			if (ImGui::BeginPopupModal("Save Layout", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
			{
				ImGui::TextUnformatted("Save the current layout as");

				if (ImGui::IsWindowAppearing())
					ImGui::SetKeyboardFocusHere();
				ImGui::SetNextItemWidth(Scaled(240.0f));
				const bool entered = ImGui::InputText("##layout_name", m_LayoutName, sizeof(m_LayoutName),
					ImGuiInputTextFlags_EnterReturnsTrue);

				const bool can_save = m_LayoutName[0] != '\0';
				ImGui::BeginDisabled(!can_save);
				const bool clicked = ImGui::Button("Save");
				ImGui::EndDisabled();

				if (can_save && (clicked || entered) && EditorLayouts::Save(m_LayoutName))
				{
					m_ActiveLayout = m_LayoutName;
					ImGui::CloseCurrentPopup();
				}

				ImGui::SameLine();
				if (ImGui::Button("Cancel"))
					ImGui::CloseCurrentPopup();

				ImGui::EndPopup();
			}
		}
		ImGui::End();
		EditorTheme::PopDockHostBackground();

		/* Style Editor：实时调参，Export 后固化回 EditorTheme.h */
		if (m_ShowStyleEditor)
			ImGui::ShowStyleEditor();
	}

	/* File：场景存取与模型导入 */
	void MainEditorLayer::ShowFileMenu()
	{
		if (!BeginBarMenu("File"))
			return;

		PanelChrome::MenuSectionHeader("SCENE");

		if (BarMenuItem("New Scene", "Ctrl+N"))
			m_Context.NewScene();
		if (BarMenuItem("Open Scene...", "Ctrl+I"))
			m_Context.ImportScene();
		/* 没有改动时保存是空操作，与工具栏保存按钮同一判据 */
		if (BarMenuItem("Save Scene", "Ctrl+S", m_Context.IsSceneDirty()))
			m_Context.SaveScene();
		if (BarMenuItem("Save Scene As...", "Ctrl+Shift+S"))
			m_Context.SaveSceneAs();

		ImGui::Separator();
		PanelChrome::MenuSectionHeader("ASSET");

		if (BarMenuItem("New Script..."))
			CreateScriptTemplate();
		if (BarMenuItem("Import Model..."))
			m_Context.ImportModel();

		ImGui::Separator();

		if (BarMenuItem("Exit"))
			Application::Instance()->Close();

		EndBarMenu();
	}

	/* Window：各窗口显隐、布局重置与编辑器外观 */
	void MainEditorLayer::ShowWindowMenu()
	{
		if (!BeginBarMenu("Window"))
			return;

		PanelChrome::MenuSectionHeader("PANELS");

		Panel::DrawToggleMenuItems(Panel::Menu::Window);

		ImGui::Separator();
		PanelChrome::MenuSectionHeader("LAYOUT");

		if (PanelChrome::MenuItemSelectWithIcon(Icons::Id::None, "Default", m_ActiveLayout.empty()))
		{
			m_ResetLayoutRequested = true;
			m_ActiveLayout.clear();
			ImGui::CloseCurrentPopup();
		}

		for (const EditorLayouts::Entry& layout : EditorLayouts::List())
		{
			if (PanelChrome::MenuItemSelectWithIcon(Icons::Id::None, layout.Name.c_str(),
				m_ActiveLayout == layout.Name))
			{
				if (EditorLayouts::Load(layout))
				{
					m_ActiveLayout = layout.Name;
					ImGui::CloseCurrentPopup();
				}
			}
		}

		ImGui::Separator();

		if (BarMenuItem("Save Layout As..."))
			m_ShowSaveLayoutPopup = true;

		ImGui::Separator();
		PanelChrome::MenuSectionHeader("APPEARANCE");

		if (BeginBarSubmenu("Theme"))
		{
			const ImGuiStyle& style = ImGui::GetStyle();
			const float box = ImGui::GetTextLineHeight() * 0.78f;
			const float gap = style.ItemInnerSpacing.x;
			const float tick = ImGui::GetFontSize() * 0.8f;
			const float row_height = ImGui::GetFrameHeight();

			/* 行宽按最长的名字定死：各行的色卡、文字、勾才会纵向对齐 */
			const float block_gap = Scaled(3.0f);
			const float pair_width = box + block_gap + box * 0.62f;
			float name_width = 0.0f;
			for (int i = 0; i < EditorTheme::ThemeCount(); ++i)
				name_width = std::max(name_width, ImGui::CalcTextSize(EditorTheme::GetTheme(i).Name).x);

			const float row_width = style.FramePadding.x * 2.0f + pair_width + gap + name_width + gap + tick;

			for (int i = 0; i < EditorTheme::ThemeCount(); ++i)
			{
				const EditorTheme::ThemePreset& preset = EditorTheme::GetTheme(i);
				const bool active = (i == EditorTheme::ActiveTheme());

				ImGui::PushID(i);
				if (ImGui::Selectable("##theme", false, ImGuiSelectableFlags_None,
					ImVec2(row_width, row_height)))
				{
					m_Settings.Theme = i;
					EditorTheme::SetTheme(i);
					EditorSettings::Save(m_Settings);
				}
				ImGui::PopID();

				const ImVec2 row_min = ImGui::GetItemRectMin();
				const ImVec2 row_max = ImGui::GetItemRectMax();
				const float center_y = (row_min.y + row_max.y) * 0.5f;
				ImDrawList* draw_list = ImGui::GetWindowDrawList();

				/* 色卡：主色一块、副色缩一号底对齐 —— 一眼看出这套主题配的是哪两个色 */
				const ImVec2 swatch_min(row_min.x + style.FramePadding.x, center_y - box * 0.5f);
				draw_list->AddRectFilled(swatch_min, ImVec2(swatch_min.x + box, swatch_min.y + box),
					ImGui::GetColorU32(preset.Accent), box * 0.3f);

				const float alt_box = box * 0.62f;
				const ImVec2 alt_min(swatch_min.x + box + block_gap, swatch_min.y + box - alt_box);
				draw_list->AddRectFilled(alt_min, ImVec2(alt_min.x + alt_box, alt_min.y + alt_box),
					ImGui::GetColorU32(preset.Alt), alt_box * 0.3f);
				/* 辅色有深有浅，描一圈细边才不会在深色底上糊掉 */
				draw_list->AddRect(alt_min, ImVec2(alt_min.x + alt_box, alt_min.y + alt_box),
					ImGui::GetColorU32(EditorTheme::Token::Border), alt_box * 0.3f, 0, 1.0f);

				draw_list->AddText(ImVec2(swatch_min.x + pair_width + gap,
					row_min.y + (row_height - ImGui::GetTextLineHeight()) * 0.5f),
				ImGui::GetColorU32(ImGuiCol_Text), preset.Name);

				if (active)
				{
					const float x = row_max.x - style.FramePadding.x - tick;
					const ImU32 tick_color = ImGui::GetColorU32(EditorTheme::Token::Accent);
					const float thickness = Scaled(1.6f);
					draw_list->AddLine(ImVec2(x, center_y),
						ImVec2(x + tick * 0.38f, center_y + tick * 0.36f), tick_color, thickness);
					draw_list->AddLine(ImVec2(x + tick * 0.38f, center_y + tick * 0.36f),
						ImVec2(x + tick, center_y - tick * 0.34f), tick_color, thickness);
				}
			}

			PanelChrome::EndMenuWithIcon();
		}

		PanelChrome::MenuItemToggleWithIcon(Icons::Id::None, "Style Editor", &m_ShowStyleEditor);

		EndBarMenu();
	}

	/* Rendering：渲染管线与画质开关（对所有视口生效） */
	void MainEditorLayer::ShowRenderingMenu()
	{
		if (!BeginBarMenu("Rendering"))
			return;

		const auto apply_settings = [this]()
		{
			EditorSettings::Apply(m_Settings);
			EditorSettings::Save(m_Settings);
		};

		PanelChrome::MenuSectionHeader("PIPELINE");

		const RenderPipeline pipeline = m_Context.GetRenderPipeline();
		if (PanelChrome::MenuItemSelectWithIcon(Icons::Id::None, "Forward", pipeline == RenderPipeline::Forward))
			m_Context.SetRenderPipeline(RenderPipeline::Forward);
		if (PanelChrome::MenuItemSelectWithIcon(Icons::Id::None, "Deferred", pipeline == RenderPipeline::Deferred))
			m_Context.SetRenderPipeline(RenderPipeline::Deferred);

		ImGui::Separator();
		PanelChrome::MenuSectionHeader("QUALITY");

		bool msaa = m_Settings.MSAA;
		if (PanelChrome::MenuItemToggleWithIcon(Icons::Id::None, "MSAA", &msaa))
		{
			m_Settings.MSAA = msaa;
			apply_settings();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("On: 4x MSAA on the scene pass, smoother object edges. Off: back to single sample.\nWith TAA enabled the geometry pass stays single sampled so depth can be sampled");

		bool gpu_skinning = m_Settings.GPUSkinning;
		if (PanelChrome::MenuItemToggleWithIcon(Icons::Id::None, "GPU Skinning", &gpu_skinning))
		{
			m_Settings.GPUSkinning = gpu_skinning;
			apply_settings();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("On: vertices are skinned in a compute shader. Off: back to CPU skinning");

		ImGui::Separator();
		PanelChrome::MenuSectionHeader("TERRAIN");

		bool terrain_vt = m_Settings.TerrainVirtualTexture;
		if (PanelChrome::MenuItemToggleWithIcon(Icons::Id::None, "Terrain Virtual Texture", &terrain_vt))
		{
			m_Settings.TerrainVirtualTexture = terrain_vt;
			apply_settings();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("On: terrain color comes from a paged cache around the camera. Off: shaded per pixel. Both look the same up close to within a couple of levels");

		/* 页池档位：页边长越大，单页越清晰、池里装得下的页越少 */
		if (BeginBarSubmenu("Page Size"))
		{
			const uint32_t sizes[] = { 128, 256, 512 };
			for (const uint32_t size : sizes)
			{
				char label[32];
				std::snprintf(label, sizeof(label), "%u px", size);
				if (PanelChrome::MenuItemSelectWithIcon(Icons::Id::None, label,
					m_Settings.TerrainVirtualTextureTexels == size))
				{
					m_Settings.TerrainVirtualTextureTexels = size;
					apply_settings();
				}
			}
			PanelChrome::EndMenuWithIcon();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Page edge in texels. The pool keeps a fixed 32MB budget, so bigger pages mean fewer of them");

		ImGui::Separator();
		PanelChrome::MenuSectionHeader("GRASS");

		bool gpu_grass = m_Settings.GpuGrassPlacement;
		if (PanelChrome::MenuItemToggleWithIcon(Icons::Id::None, "GPU Grass Placement", &gpu_grass))
		{
			m_Settings.GpuGrassPlacement = gpu_grass;
			apply_settings();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("On: blade positions are generated in a compute shader. Off: back to per-blade CPU placement. Needs compute shader support");

		bool gpu_grass_indirect = m_Settings.GpuGrassIndirect;
		if (PanelChrome::MenuItemToggleWithIcon(Icons::Id::None, "GPU Grass Indirect Draw", &gpu_grass_indirect))
		{
			m_Settings.GpuGrassIndirect = gpu_grass_indirect;
			apply_settings();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("On: grass is culled on the GPU and drawn with one indirect call. Off: one submit per chunk. Needs compute shader support");

		/* 关掉就是逐块提交，用来对比 draw call */
		bool grass_batching = m_Settings.GrassBatching;
		if (PanelChrome::MenuItemToggleWithIcon(Icons::Id::None, "Grass Block Batching", &grass_batching))
		{
			m_Settings.GrassBatching = grass_batching;
			apply_settings();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("On: adjacent chunks are merged into one draw by instance budget. Off: one submit per chunk. No effect while indirect draw is on");

		/* 草密度：连续值留在编辑器设置里，菜单只给三档 */
		if (BeginBarSubmenu("Grass Density"))
		{
			const float density = m_Settings.GrassDensityScale;
			const struct { const char* Label; float Value; } levels[] = {
				{ "Low", 0.5f }, { "Normal", 1.0f }, { "High", 1.5f },
			};
			for (const auto& level : levels)
			{
				if (PanelChrome::MenuItemSelectWithIcon(Icons::Id::None, level.Label,
					std::abs(density - level.Value) < 0.01f))
				{
					m_Settings.GrassDensityScale = level.Value;
					apply_settings();
				}
			}
			PanelChrome::EndMenuWithIcon();
		}

		EndBarMenu();
	}

	/* Scripts：C++ 脚本模块的编译与热重载 */
	void MainEditorLayer::ShowScriptsMenu()
	{
		if (!BeginBarMenu("Scripts"))
			return;

		const auto apply_settings = [this]()
		{
			EditorSettings::Apply(m_Settings);
			EditorSettings::Save(m_Settings);
		};

		if (BarMenuItem("Recompile Scripts"))
			m_Context.ReloadScripts([this]() { return m_ScriptModule.Recompile(); });
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Compile the project scripts and hot-reload the module");

		if (BarMenuItem("Reload Module"))
			m_Context.ReloadScripts([this]() { return m_ScriptModule.LoadLatest(); });
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Load the newest built module and migrate live instances to it");

		ImGui::Separator();

		bool auto_reload = m_Settings.AutoReloadScripts;
		if (PanelChrome::MenuItemToggleWithIcon(Icons::Id::None, "Auto Reload", &auto_reload))
		{
			m_Settings.AutoReloadScripts = auto_reload;
			m_ScriptModule.SetAutoReloadEnabled(auto_reload);
			apply_settings();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Reload the module as soon as a new build shows up");

		bool auto_compile = m_Settings.AutoCompileScripts;
		if (PanelChrome::MenuItemToggleWithIcon(Icons::Id::None, "Auto Compile On Open", &auto_compile))
		{
			m_Settings.AutoCompileScripts = auto_compile;
			m_ScriptModule.SetAutoCompileEnabled(auto_compile);
			apply_settings();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Build the project scripts on open when no module exists yet");

		ImGui::Separator();

		/* 脚本执行顺序：全场景一张表，Order 小的先跑 */
		Panel::DrawToggleMenuItems(Panel::Menu::Scripts);

		ImGui::Separator();
		PanelChrome::MenuStatusText(m_ScriptModule.GetStatus().c_str());

		char class_count[64];
		std::snprintf(class_count, sizeof(class_count), "%d class(es) registered",
			m_ScriptModule.GetClassCount());
		PanelChrome::MenuStatusText(class_count);

		EndBarMenu();
	}

	/* Shaders：着色器热重载 */
	void MainEditorLayer::ShowShadersMenu()
	{
		if (!BeginBarMenu("Shaders"))
			return;

		const auto apply_settings = [this]()
		{
			EditorSettings::Apply(m_Settings);
			EditorSettings::Save(m_Settings);
		};

		if (BarMenuItem("Reload All"))
		{
			const size_t reloaded = m_ShaderReloader.ReloadAll();
			EDITOR_LOG_INFO("[Shader] Reloaded {} of {} cached shaders", reloaded,
				AssetManager::Instance().GetCount<DeviceShader>());
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Reload every cached shader whose files differ from the loaded version, ignoring timestamps");

		if (BarMenuItem("Reload Changed"))
			m_ShaderReloader.ReloadChanged();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Reload only the shaders whose files changed");

		ImGui::Separator();

		bool auto_reload_shaders = m_Settings.AutoReloadShaders;
		if (PanelChrome::MenuItemToggleWithIcon(Icons::Id::None, "Auto Reload", &auto_reload_shaders))
		{
			m_Settings.AutoReloadShaders = auto_reload_shaders;
			m_ShaderReloader.SetAutoReloadEnabled(auto_reload_shaders);
			apply_settings();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Reload a shader as soon as its file or an included file changes");

		ImGui::Separator();

		char shader_status[96];
		std::snprintf(shader_status, sizeof(shader_status), "%d cached, %d queued",
			static_cast<int>(AssetManager::Instance().GetCount<DeviceShader>()),
			static_cast<int>(m_ShaderReloader.GetPendingCount()));
		PanelChrome::MenuStatusText(shader_status);

		EndBarMenu();
	}

	/* 顶部工具栏依次排列文件操作和变换工具，运行按钮居中，视图工具贴右。 */
	void MainEditorLayer::ShowToolbarUI()
	{
		PROFILE_FUNCTION();

		const float kToolbarHeight = Scaled(34.0f);
		const float kButtonSize    = Scaled(24.0f);
		const float kGroupGap      = Scaled(9.0f);
		const float kSidePadding   = Scaled(8.0f);

		ImGui::PushStyleColor(ImGuiCol_ChildBg, EditorTheme::Token::Neutral3);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(Scaled(2.0f), 0.0f));

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

			/* 保存按钮只在场景有未保存改动时可用（与层级面板根节点的脏标记同一判据） */
			ImGui::SameLine();
			ImGui::BeginDisabled(!m_Context.IsSceneDirty());
			if (Icons::IconButton(Icons::Id::Save, button_size, false, "Save Scene  (Ctrl+S)"))
				m_Context.SaveScene();
			ImGui::EndDisabled();

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
			if (Icons::IconButton(Icons::Id::Translate, button_size, gizmo_type == 0, "Translate  (W)"))
				m_Context.SetGizmoType(0);

			ImGui::SameLine();
			if (Icons::IconButton(Icons::Id::Rotate, button_size, gizmo_type == 1, "Rotate  (E)"))
				m_Context.SetGizmoType(1);

			ImGui::SameLine();
			if (Icons::IconButton(Icons::Id::Scale, button_size, gizmo_type == 2, "Scale  (R)"))
				m_Context.SetGizmoType(2);

			/* 坐标空间：局部 / 世界两格图标开关，比下拉框少一次点击 */
			ImGui::SameLine(0.0f, Scaled(6.0f));
			{
				const Icons::Id space_icons[2] = { Icons::Id::LocalSpace, Icons::Id::WorldSpace };
				const char* space_tips[2] = { "Local Space", "World Space" };
				const int space_index = Icons::IconToggleGroup("##GizmoSpace", space_icons, space_tips, 2,
					m_Context.IsGizmoLocalSpace() ? 0 : 1,
					ImVec2(kButtonSize * 2.0f + Scaled(6.0f), kButtonSize));
				if (space_index >= 0)
					m_Context.SetGizmoLocalSpace(space_index == 0);
			}

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

			const float utility_group_width = kButtonSize * 3.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f;
			const float utility_group_x = ImGui::GetContentRegionMax().x - utility_group_width;
			const float play_right = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
			ImGui::SameLine(std::max(play_right + kGroupGap, utility_group_x));

			bool& camera_visible = Panel::VisibleFlag(Panel::kEditorCamera);
			if (Icons::IconButton(Icons::Id::Camera, button_size, camera_visible, "Editor Camera Parameters"))
				camera_visible = !camera_visible;

			ImGui::SameLine();
			const char* debug_view_label = DebugViewModeLabel(m_Context.GetDebugViewMode());
			const std::string debug_view_tip =
				std::string("Debug View") + (debug_view_label != nullptr ? std::string(": ") + debug_view_label : std::string());
			const bool debug_menu_open = ImGui::IsPopupOpen("##DebugViewMenu");
			if (Icons::IconButton(Icons::Id::DebugView, button_size, debug_view_label != nullptr, debug_view_tip.c_str())
				&& !debug_menu_open)
			{
				ImGui::OpenPopup("##DebugViewMenu");
			}

			/* 弹层锚在按钮正下方、右缘对齐（与 Gizmos 菜单同款锚法）；最小宽度取最长行
			 * 标签 + 图标与勾选列的固定组分 —— 各行勾选列与分区头细线从首帧起就对位 */
			const ImVec2 debug_menu_button_max = ImGui::GetItemRectMax();
			const float debug_menu_min_width = ImGui::CalcTextSize("Ambient Occlusion").x
				+ ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemInnerSpacing.x * 2.0f
				+ ImGui::GetFontSize() * 2.0f + Scaled(24.0f);

			ImGui::SetNextWindowPos(ImVec2(debug_menu_button_max.x, debug_menu_button_max.y + Scaled(4.0f)),
				ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
			ImGui::SetNextWindowSizeConstraints(ImVec2(debug_menu_min_width, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
			if (ImGui::BeginPopup("##DebugViewMenu"))
			{
				DrawDebugViewMenuItems(m_Context);
				ImGui::EndPopup();
			}

			/* Gizmos 紧随调试按钮、保持贴最右（弹层锚法不变） */
			ImGui::SameLine();

			const bool gizmo_menu_open = ImGui::IsPopupOpen("##GizmoMenu");
			if (Icons::IconButton(Icons::Id::Gizmos, button_size, gizmo_menu_open, "Gizmos") && !gizmo_menu_open)
				ImGui::OpenPopup("##GizmoMenu");

			/* 弹层锚在按钮正下方、右缘对齐（pivot 取右上角）——按钮贴着工具栏右缘，
			 * 左对齐展开会伸出窗口；最小宽度取最长行标题 + 图标与勾选列的固定组分，
			 * 撑足后各行的勾选列落在同一纵向基线上 */
			const ImVec2 gizmo_button_max = ImGui::GetItemRectMax();
			const float gizmo_menu_min_width = ImGui::CalcTextSize("Reflection Probes").x
				+ ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetStyle().ItemInnerSpacing.x * 2.0f
				+ ImGui::GetFontSize() * 2.0f + Scaled(24.0f);

			ImGui::SetNextWindowPos(ImVec2(gizmo_button_max.x, gizmo_button_max.y + Scaled(4.0f)),
				ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
			ImGui::SetNextWindowSizeConstraints(ImVec2(gizmo_menu_min_width, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
			if (ImGui::BeginPopup("##GizmoMenu"))
			{
				auto& gizmo_options = GetViewportGizmoOptions();

				/* 全选行（纯三态复选框，不加图标与文案）：全开 = 勾、部分开 = 横杠、
				 * 全关 = 空框；点击 = 全开 ↔ 全关（部分状态点击补齐为全开）—— 各分项
				 * 状态互相独立，这一行只是聚合显示与批量开关 */
				const bool all_on = gizmo_options.ShowGrid && gizmo_options.ShowWorldAxis
					&& gizmo_options.ShowLight && gizmo_options.ShowCamera
					&& gizmo_options.ShowReflectionProbe && gizmo_options.ShowSprite;
				const bool none_on = !gizmo_options.ShowGrid && !gizmo_options.ShowWorldAxis
					&& !gizmo_options.ShowLight && !gizmo_options.ShowCamera
					&& !gizmo_options.ShowReflectionProbe && !gizmo_options.ShowSprite;
				if (PanelChrome::MenuItemTristateBox(all_on, none_on))
				{
					const bool target = !all_on;
					gizmo_options.ShowGrid = target;
					gizmo_options.ShowWorldAxis = target;
					gizmo_options.ShowLight = target;
					gizmo_options.ShowCamera = target;
					gizmo_options.ShowReflectionProbe = target;
					gizmo_options.ShowSprite = target;
				}
				ImGui::Separator();

				PanelChrome::MenuItemToggleWithIcon(Icons::Id::Grid, "Grid", &gizmo_options.ShowGrid);
				PanelChrome::MenuItemToggleWithIcon(Icons::Id::WorldAxis, "World Axis", &gizmo_options.ShowWorldAxis);
				ImGui::Separator();
				PanelChrome::MenuItemToggleWithIcon(Icons::Id::Light, "Lights", &gizmo_options.ShowLight);
				PanelChrome::MenuItemToggleWithIcon(Icons::Id::Camera, "Cameras", &gizmo_options.ShowCamera);
				PanelChrome::MenuItemToggleWithIcon(Icons::Id::ReflectionProbe, "Reflection Probes",
					&gizmo_options.ShowReflectionProbe);
				PanelChrome::MenuItemToggleWithIcon(Icons::Id::Sprite, "Sprites", &gizmo_options.ShowSprite);
				ImGui::EndPopup();
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

	void MainEditorLayer::ShowEditorCameraUI()
	{
		bool& visible = Panel::VisibleFlag(Panel::kEditorCamera);
		if (!visible)
			return;

		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - Scaled(16.0f),
			viewport->WorkPos.y + Scaled(70.0f)), ImGuiCond_FirstUseEver, ImVec2(1.0f, 0.0f));
		ImGui::SetNextWindowSize(ImVec2(Scaled(500.0f), Scaled(640.0f)), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSizeConstraints(ImVec2(Scaled(360.0f), Scaled(240.0f)), ImVec2(FLT_MAX, FLT_MAX));
		PanelWindow window(Panel::kEditorCamera, &visible);
		if (!window)
			return;

		const EditorCamera* camera = m_Context.GetEditorCamera();
		if (camera == nullptr)
		{
			ImGui::TextDisabled("Scene editor camera is not available.");
			return;
		}

		const auto row = [](const char* label, const char* format, auto... values)
		{
			char text[256];
			std::snprintf(text, sizeof(text), format, values...);
			const float width = ImGuiExt::BeginPropertyRow(label, Scaled(122.0f));
			PanelChrome::DrawClippedTextLine(text, width);
			if (ImGui::IsItemHovered() && ImGui::CalcTextSize(text).x > width)
				ImGui::SetTooltip("%s", text);
			ImGuiExt::EndPropertyRow();
		};
		const auto vector_row = [&](const char* label, const glm::vec3& value)
		{
			row(label, "X %.3f   Y %.3f   Z %.3f", value.x, value.y, value.z);
		};

		ImGui::TextDisabled("Scene viewport camera   |   Live   |   Read-only");
		const auto transform = PanelChrome::BeginCard("Transform");
		if (transform.Open)
		{
			vector_row("Position", camera->GetPosition());
			vector_row("Pivot", camera->GetFocalPoint());
			row("Yaw", "%.3f deg", glm::degrees(camera->GetYaw()));
			row("Pitch", "%.3f deg", glm::degrees(camera->GetPitch()));
			row("Orbit Distance", "%.3f", camera->GetDistance());
		}
		PanelChrome::EndCard(transform);

		const auto projection = PanelChrome::BeginCard("Projection");
		if (projection.Open)
		{
			row("Type", "%s", "Perspective");
			row("Vertical FOV", "%.3f deg", camera->GetFov());
			row("Aspect Ratio", "%.6f", camera->GetAspectRatio());
			row("Near Clip", "%.6g", camera->GetNearClip());
			row("Far Clip", "%.6g", camera->GetFarClip());
			const auto& region = camera->GetViewportRegion();
			row("Render Size", "%u x %u px", region.Width, region.Height);
			row("Depth", "%s", "Reversed Z");
		}
		PanelChrome::EndCard(projection);

		const auto navigation = PanelChrome::BeginCard("Navigation");
		if (navigation.Open)
		{
			const char* state = camera->IsFlying() ? "Fly" : camera->IsOrbiting() ? "Orbit"
				: camera->IsNavigating() ? "Pan / Zoom" : "Idle";
			row("Gesture", "%s", state);
			row("Base Fly Speed", "%.3f", camera->GetMoveSpeed());
		}
		PanelChrome::EndCard(navigation);

		const auto directions = PanelChrome::BeginCard("Directions", Icons::Id::None,
			Icons::Id::None, nullptr, false);
		if (directions.Open)
		{
			vector_row("Forward", camera->GetForwardDir());
			vector_row("Right", camera->GetRightDir());
			vector_row("Up", camera->GetUpDir());
			const glm::quat orientation = camera->GetOrientation();
			row("Quaternion", "X %.4f   Y %.4f   Z %.4f   W %.4f",
				orientation.x, orientation.y, orientation.z, orientation.w);
		}
		PanelChrome::EndCard(directions);

		const auto matrices = PanelChrome::BeginCard("Matrices", Icons::Id::None,
			Icons::Id::None, nullptr, false);
		if (matrices.Open)
		{
			const auto matrix_rows = [&](const char* label, const glm::mat4& matrix)
			{
				ImGui::PushID(label);
				ImGui::TextDisabled("%s", label);
				for (int index = 0; index < 4; ++index)
				{
					char row_label[16];
					std::snprintf(row_label, sizeof(row_label), "Row %d", index);
					row(row_label, "%.6g   %.6g   %.6g   %.6g",
						matrix[0][index], matrix[1][index], matrix[2][index], matrix[3][index]);
				}
				ImGui::PopID();
			};
			matrix_rows("View", camera->GetViewMatrix());
			matrix_rows("Projection", camera->GetProjectionMatrix());
			matrix_rows("View Projection", camera->GetViewProjectionMatrix());
		}
		PanelChrome::EndCard(matrices);
	}

	void MainEditorLayer::OnUpdate(float delta_time)
	{
		PROFILE_FUNCTION();

		/* IDE 那边编好的脚本产物自动跟上：轮询产物时间戳，编译成功才重载 */
		m_ScriptModule.TickAutoReload(delta_time);

		/* 着色器同理：盯依赖文件的修改时间，改了就地重载（帧边界提交，不打断绘制） */
		m_ShaderReloader.Tick(delta_time);
	}
}
