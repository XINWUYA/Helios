#include "Pch.h"
#include "SceneEditorLayer.h"
#include <glm/gtc/type_ptr.hpp>
#include "EditorBuiltinCamera.h"
#include "EditorIcons.h"
#include "PanelChrome.h"
#include "PanelRegistry.h"
#include "Command/TransformCommand.h"
#include "Helios/Application/Application.h"
#include "ImGuizmo.h"

namespace Helios
{
	SceneEditorLayer::SceneEditorLayer()
		: ILayer("SceneEditorLayer")
	{
		PROFILE_FUNCTION();
	}

	void SceneEditorLayer::OnAttached()
	{
		PROFILE_FUNCTION();
		
		// Camera
		m_pEditorCamera = CreateUniquePtr<EditorCamera>(30.0f);

		m_pMainScene = CreateSharedPtr<Scene>();
		m_SceneHierarchy.SetOwnerScene(m_pMainScene);

		/* 属性面板的字段改动与撤销重做共用同一条历史 */
		m_SceneHierarchy.SetCommandStack(&m_CommandStack);






	}

	void SceneEditorLayer::OnDetached()
	{
		PROFILE_FUNCTION();

		ILayer::OnDetached();
	}

	void SceneEditorLayer::OnImGuiRender()
	{
		PROFILE_FUNCTION();

		if (!m_IsActivated)
			return;
		
		/* 统计信息 */
		ShowStatisticInfoUI();

		/* 脏标记每帧推送：场景名旁的圆点必须和编辑历史同步，不能缓存成独立状态 */
		m_SceneHierarchy.SetSceneDirty(IsSceneDirty());

		/* 场景管理及属性窗口 */
		m_SceneHierarchy.OnImGuiRender();

		/* 主窗口，需要最后再画，以确保能够得到正确的窗口宽高 */
		ShowSceneViewportUI();
	}

	void SceneEditorLayer::OnEvent(IEvent* event)
	{
		PROFILE_FUNCTION();

		if (!m_IsActivated || !event)
			return;

		m_pEditorCamera->OnEvent(event);

		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<KeyPressedEvent>(BIND_EVENT_FUNC(SceneEditorLayer::OnKeyPressed));
		dispatcher.Dispatch<MouseButtonPressedEvent>(BIND_EVENT_FUNC(SceneEditorLayer::OnMouseButtonPressed));
	}

	void SceneEditorLayer::UpdateViewport()
	{
		PROFILE_FUNCTION();

		//const FrameBufferDescription desc = m_pFrameBuffer->GetDescription();
		if (m_ViewportRegion.Width > 0 && m_ViewportRegion.Height > 0/* && (desc.Width != m_ViewportRegion.Width() || desc.Height != m_ViewportRegion.Height())*/)
		{
			/* ImGui 给的是逻辑点，RenderTarget 必须按物理像素分配（不然 Retina 上视口只渲染 1/2 分辨率、
			 * 被拉伸）。ClampRenderSize 负责负值 / 超大值的防护。 */
			const auto content_scale = Application::Instance()->GetWindow().GetContentScale();
			const auto rt_size = ClampRenderSize(
				static_cast<float>(m_ViewportRegion.Width),
				static_cast<float>(m_ViewportRegion.Height),
				content_scale);
			m_pEditorCamera->SetViewportRegion({ 0, 0, rt_size.x, rt_size.y });
			//m_pOrthographicCameraController->OnResize(static_cast<float>(m_ViewportRegion.Width()), static_cast<float>(m_ViewportRegion.Height()));
		}
	}

	bool SceneEditorLayer::OnKeyPressed(KeyPressedEvent* event)
	{
		PROFILE_FUNCTION();

		if (event->GetRepeatCount() > 0)
			return false;

		bool is_ctrl_pressed = Input::IsKeyPressed(Key::LeftControl) || Input::IsKeyPressed(Key::RightControl);
		bool is_shift_pressed = Input::IsKeyPressed(Key::LeftShift) || Input::IsKeyPressed(Key::RightShift);
		bool is_button_right_pressed = Input::IsMouseButtonPressed(Mouse::ButtonRight);

		switch (event->GetKeyCode())
		{
		case Key::N: /* Ctrl+N：新建一个场景 */
			{
				if (is_ctrl_pressed)
					NewScene();
			}
			return true;
		case Key::S: /* Ctrl+S: 保存场景; Ctrl+Shift+S: 场景另存为 */
			if (is_ctrl_pressed)
			{
				if (is_shift_pressed)
					SaveSceneAs();
				else
					SaveScene();
			}
			return true;
		case Key::I: /* Ctrl+I：导入一个场景 */
			{
				if (is_ctrl_pressed)
					ImportScene();
			}
			return true;;
		case Key::Z: /* Ctrl+Z：撤销；Ctrl+Shift+Z：重做 */
			if (is_ctrl_pressed)
			{
				if (is_shift_pressed)
					Redo();
				else
					Undo();
			}
			return true;
		case Key::Y: /* Ctrl+Y：重做 */
			if (is_ctrl_pressed)
				Redo();
			return true;
		}

		return false;
	}

	bool SceneEditorLayer::OnMouseButtonPressed(MouseButtonPressedEvent* event)
	{
		PROFILE_FUNCTION();

		if (event->GetMouseButton() == Mouse::ButtonLeft)
		{
			CheckMouseSelectEntity();

			if (m_IsViewportHovered && !ImGuizmo::IsOver() && !Input::IsKeyPressed(Key::LeftAlt))
				m_SceneHierarchy.SetSelectedEntity(m_HoveredEntity);
		}
		return false;
	}

	/* 响应拖拽文件到主窗口 */
	void SceneEditorLayer::OnDragItemToScene(const std::filesystem::path& path)
	{
		PROFILE_FUNCTION();

		const auto extension = PathToUtf8(path.extension());

		/* 场景文件 */
		if (extension == ".scn")
		{
			const std::string scene_path = PathToUtf8(path);
			EDITOR_LOG_DEBUG("Import scene file: {}.", scene_path);

			SharedPtr<Scene> new_scene = CreateSharedPtr<Scene>();
			if (new_scene->Deserializer(scene_path))
				SetActiveScene(new_scene, scene_path);

			return;
		}

		/* 模型文件 */
		if (extension == ".mesh")
		{
			const std::string model_path = PathToUtf8(path);
			EDITOR_LOG_DEBUG("Import model file: {}.", model_path);

			Entity entity = m_pMainScene->CreateEntity(PathToUtf8(path.stem()));
			auto& model_component = entity.AddComponent<ModelComponent>();
			model_component.m_Model = Model::Create(model_path);

			/* 这条改动不走命令栈（没有对应的撤销），脏标记只能在这里显式补上 */
			m_HasUnrecordedChange = true;

			return;
		}
		
	}

	void SceneEditorLayer::SetActiveScenePath(const std::string& path)
	{
		PROFILE_FUNCTION();

		m_ActiveScenePath = path;

		/* 层级面板的根节点就是当前场景名：路径改了必须同步，否则另存为之后显示的仍是旧名字 */
		m_SceneHierarchy.SetScenePath(path);
	}

	bool SceneEditorLayer::IsSceneDirty() const
	{
		return m_HasUnrecordedChange || m_CommandStack.GetSceneEditCount() != m_SavedSceneEditCount;
	}

	void SceneEditorLayer::MarkSceneSaved()
	{
		m_SavedSceneEditCount = m_CommandStack.GetSceneEditCount();
		m_HasUnrecordedChange = false;
	}

	void SceneEditorLayer::SetActiveScene(const SharedPtr<Scene>& scene, const std::string& path)
	{
		PROFILE_FUNCTION();

		m_pMainScene = scene;
		SetActiveScenePath(path);

		/* 层级面板与 RenderView 都要指向新场景 */
		m_SceneHierarchy.SetOwnerScene(m_pMainScene);
		m_pEditorCamera->GetRenderView()->SetOwnerScene(m_pMainScene);

		/* 场景内容已整体替换：缓存的实体句柄（悬停 / 选中）与旧命令都不再有效 */
		m_HoveredEntity = {};
		m_CommandStack.Clear();
		m_IsGizmoDragging = false;

		/* 历史刚被清空（没保存过的新场景也一样）：当前位置就是干净点 */
		MarkSceneSaved();

		/* 新建 / 打开场景后场景视口必须可见：它同时是视口窗口被关闭后的恢复入口，
		 * 否则用户新建了一个空场景却看不到它。 */
		m_IsActivated = true;
		m_IsViewportVisible = true;
	}

	void SceneEditorLayer::NewScene()
	{
		PROFILE_FUNCTION();

		SetActiveScene(CreateSharedPtr<Scene>(), std::string());
	}

	void SceneEditorLayer::ImportScene()
	{
		PROFILE_FUNCTION();

		const auto file_path = FileDialog::OpenFile("scene(*.scn)\0*.scn\0");
		if (file_path.empty())
			return;

		m_pMainScene->Deserializer(file_path);
		SetActiveScene(m_pMainScene, file_path);
	}

	void SceneEditorLayer::SaveScene()
	{
		PROFILE_FUNCTION();

		/* 若当前场景从未保存过，需要先确定一个保存路径 */
		if (m_ActiveScenePath.empty())
			SetActiveScenePath(FileDialog::SaveFile("scene(*.scn)\0*.scn\0"));

		/* 用户取消了路径选择：不写出空路径 */
		if (m_ActiveScenePath.empty())
			return;

		m_pMainScene->Serializer(m_ActiveScenePath);

		/* 场景内容与文件一致了：当前位置成为新的干净点 */
		MarkSceneSaved();
	}

	/* 保存场景到指定路径 */
	void SceneEditorLayer::SaveSceneAs()
	{
		PROFILE_FUNCTION();

		const std::string file_path = FileDialog::SaveFile("scene(*.scn)\0*.scn\0");

		/* 用户取消：保持当前场景路径不变 */
		if (file_path.empty())
			return;

		m_pMainScene->Serializer(file_path);

		/* 保存成功后，当前场景路径切换为新路径 */
		SetActiveScenePath(file_path);
		MarkSceneSaved();
	}

	bool SceneEditorLayer::Undo()
	{
		PROFILE_FUNCTION();

		return m_CommandStack.Undo();
	}

	bool SceneEditorLayer::Redo()
	{
		PROFILE_FUNCTION();

		return m_CommandStack.Redo();
	}

	void SceneEditorLayer::ShowSceneViewportUI()
	{
		PROFILE_FUNCTION();

		//m_ViewportRegion = { 268, 2188, 317, 1371 };
		static ImGuiWindowFlags tab_bar_flags = ImGuiWindowFlags_NoFocusOnAppearing;

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		/* 每帧都必须调用 Begin：窗口被关闭（点 X 或 View 菜单取消勾选）时它返回 false，
		 * 只有继续调用才可能重新显示（关闭时不画内容即可）。 */
		if (ImGui::Begin(Panel::kScene, &m_IsViewportVisible))
		{
			/* 获取窗口范围 */
			const auto viewport_region_min = ImGui::GetWindowContentRegionMin();
			const auto viewport_region_max = ImGui::GetWindowContentRegionMax();
			const auto viewport_offset = ImGui::GetWindowPos();
			m_ViewportRegion.MinX = viewport_region_min.x + viewport_offset.x;
			m_ViewportRegion.MinY = viewport_region_min.y + viewport_offset.y;
			/* 窗口极小时 max 可能小于 min，差值为负；赋给 uint32_t 会回绕成极大值，
			 * 因此先在浮点域夹到非负再赋值（否则下游按"尺寸有效"处理会分配巨型 RT）。 */
			m_ViewportRegion.Width = static_cast<uint32_t>(std::max(0.0f, viewport_region_max.x - viewport_region_min.x));
			m_ViewportRegion.Height = static_cast<uint32_t>(std::max(0.0f, viewport_region_max.y - viewport_region_min.y));

			/* 若当前ImGui窗口不是主窗口，应阻塞事件传递 */
			m_IsViewportFocused = ImGui::IsWindowFocused();
			m_IsViewportHovered = ImGui::IsWindowHovered();
			// Application::Instance()->GetImGuiLayer()->BlockEvents(!m_IsViewportFocused && !m_IsViewportHovered);

			/* 绘制场景 */
			auto output_rt = m_pEditorCamera->GetRenderView()->GetRenderTarget();
			if (output_rt)
			{
				const auto viewport_panel_size = ImGui::GetContentRegionAvail();
				/* ImTextureID 统一存放 DeviceTexture 指针：GetTextureID() 返回的硬件
				 * 句柄在 Metal 上是 64 位指针被截断后的 32 位，不能当指针使用。 */
				ImGui::Image((ImTextureID)output_rt.get(), viewport_panel_size, ImVec2{ 0, 1 }, ImVec2{ 1, 0 });
			}

			/* 拖动资源到主窗口 */
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RESOURCE_BROWSER_ITEM"))
				{
					std::filesystem::path relative_path;
					if (payload->DataSize > 1 && TryPathFromUtf8Payload(
						payload->Data, static_cast<size_t>(payload->DataSize), relative_path))
						OnDragItemToScene(g_AssetsPath / relative_path);
				}
				ImGui::EndDragDropTarget();
			}

			/* Gizmos */
			ShowOperationGizmoUI();
		}
		else
		{
			/* 视口窗口不可见：清掉交互标志，否则残留的 focus / hover
			 * 会让相机继续吃键鼠输入、点击也会被误判成视口内选择。 */
			m_IsViewportFocused = false;
			m_IsViewportHovered = false;
		}

		ImGui::End();
		ImGui::PopStyleVar();
	}

	void SceneEditorLayer::ShowStatisticInfoUI()
	{
		PROFILE_FUNCTION();

		ImGui::Begin(Panel::kStatInfo);
		{
			const PanelChrome::HeaderRow header = PanelChrome::BeginHeaderRow(Icons::Id::Stats);
			PanelChrome::DrawHeaderTitle(header, "Render Stats");
			PanelChrome::EndHeaderRow(header);

			ShowGPUTimingsCard();
		}
		ImGui::End();
	}

	/* GPU 计时：开关 + 逐层耗时表。
	 * 表放在卡片里，耗时列右对齐 —— 数字对得齐，才一眼看得出哪一级最贵。 */
	void SceneEditorLayer::ShowGPUTimingsCard()
	{
		PROFILE_FUNCTION();

		const PanelChrome::Card card = PanelChrome::BeginCard("GPU Timings", Icons::Id::Stats);

		if (card.Open)
		{
			/* GPU 计时器的开关：面板级开关，占卡身的一行 */
			bool timer_enabled = RenderQueryProfiler::Instance().IsEnabled();
			if (ImGuiExt::DrawCheckboxUI("GPU Timer", timer_enabled))
				RenderQueryProfiler::Instance().SetEnabled(timer_enabled);

			const ResultGPUTimerNode& timer_root = Renderer::GetGPUTimerRoot();
			if (timer_root.Label.empty())
			{
				ImGuiExt::DrawCommonTextUI("Timers", "No data");
			}
			else
			{
				std::function<void(const ResultGPUTimerNode&)> show_node_recursively;
				show_node_recursively = [&show_node_recursively](const ResultGPUTimerNode& timer_node)
					{
						if (timer_node.Label.empty())
							return;

						ImGui::TableNextRow();
						ImGui::TableNextColumn();

						char label[64] = {};
						snprintf(label, sizeof(label), "(%d) %s", timer_node.QueryIndex, timer_node.Label.c_str());
						label[sizeof(label) - 1] = 0;

						const bool has_children = !timer_node.Children.empty();
						ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_DefaultOpen;
						if (!has_children)
							flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_Bullet;
						else
							flags |= ImGuiTreeNodeFlags_OpenOnArrow;

						const bool is_opened = ImGui::TreeNodeEx(
							reinterpret_cast<void*>(static_cast<intptr_t>(timer_node.QueryIndex)), flags, "%s", label);

						ImGui::TableNextColumn();
						ImGui::Text("%.2f", timer_node.GPUTime);

						if (is_opened && has_children)
						{
							for (const auto& child : timer_node.Children)
								show_node_recursively(child);

							ImGui::TreePop();
						}
					};

				/* 表头 + 行背景由主题配色；耗时列按"最长数字"定宽，避免列宽随帧抖动 */
				const ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg
					| ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoBordersInBody
					| ImGuiTableFlags_SizingFixedFit;

				if (ImGui::BeginTable("GPUTimings", 2, table_flags))
				{
					ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupColumn("us", ImGuiTableColumnFlags_WidthFixed,
						ImGui::CalcTextSize("00000.00").x);

					ImGui::TableHeadersRow();
					show_node_recursively(timer_root);
					ImGui::EndTable();
				}
			}
		}

		PanelChrome::EndCard(card);

		// todo: 3D 统计（三角形数 / DrawCall / 模型数）准备好数据源后在这里补一张卡
	}

	void SceneEditorLayer::ShowOperationGizmoUI()
	{
		PROFILE_FUNCTION();

		// Editor camera
		const glm::mat4& projection_mat = m_pEditorCamera->GetProjectionMatrix();
		glm::mat4 view_mat = m_pEditorCamera->GetViewMatrix();

		ImGuizmo::SetOrthographic(false);
		ImGuizmo::SetDrawlist();
		ImGuizmo::SetRect(m_ViewportRegion.MinX, m_ViewportRegion.MinY, m_ViewportRegion.Width, m_ViewportRegion.Height);

		Entity selected_entity = m_SceneHierarchy.GetSelectedEntity();
		/* 组件可被移除，缺少 Transform 时本帧不画 Gizmo */
		if (selected_entity && m_GizmoType != -1 && m_pMainScene && selected_entity.HasComponent<TransformComponent>())
		{
			// Entity transform
			auto& transform_component = selected_entity.GetComponent<TransformComponent>();

			/* Gizmo 作用在世界变换上：实体挂在父节点下时，本地变换不能直接当世界位置用。
			 * 无父节点时父世界变换是单位阵，与既有行为完全一致。 */
			glm::mat4 transform_mat = m_pMainScene->GetWorldTransform(selected_entity);
			const glm::mat4 parent_world = m_pMainScene->GetWorldTransform(m_pMainScene->GetParent(selected_entity));

			// Snapping
			bool snap = Input::IsKeyPressed(Key::LeftControl);
			float snap_value = 0.5f; // Snap to 0.5m for translation/scale
			// Snap to 45 degrees for rotation
			if (m_GizmoType == ImGuizmo::OPERATION::ROTATE)
				snap_value = 45.0f;

			float snap_values[3] = { snap_value, snap_value, snap_value };

			ImGuizmo::Manipulate(glm::value_ptr(view_mat), glm::value_ptr(projection_mat),
				(ImGuizmo::OPERATION)m_GizmoType, ImGuizmo::LOCAL, glm::value_ptr(transform_mat),
				nullptr, snap ? snap_values : nullptr); /* ImGuizmo::LOCAL/ ImGuizmo::WORLD */

			const bool is_using = ImGuizmo::IsUsing();

			/* 按下时开启合并窗口：整段拖拽的逐帧改动合并为一条历史 */
			if (is_using && !m_IsGizmoDragging)
			{
				m_IsGizmoDragging = true;
				m_CommandStack.BeginTransaction();
			}

			if (is_using)
			{
				/* 从变换矩阵中恢复：ImGuizmo 给的是世界变换，先按父空间换算回本地再分解，
				 * 否则挂在父节点下的实体会随父节点一起偏移。 */
				glm::vec3 position, rotation, scale;
				DecomposeTransform(glm::inverse(parent_world) * transform_mat, position, rotation, scale);

				/* 旋转按增量叠加（ImGuizmo 给出的是绝对量） */
				TransformComponent next = transform_component;
				next.m_Position = position;
				next.m_Rotation += rotation - transform_component.m_Rotation;
				next.m_Scale = scale;

				/* 改动经命令栈落地 */
				m_CommandStack.Execute(CreateUniquePtr<TransformCommand>(selected_entity, transform_component, next));
			}
			else if (m_IsGizmoDragging)
			{
				m_IsGizmoDragging = false;
				m_CommandStack.EndTransaction();
			}
		}

		/* 坐标线框（todo：划到场景物体之后） */
		//const auto identity_mat = glm::identity<glm::mat4>();
		//ImGuizmo::DrawGrid(glm::value_ptr(view_mat), glm::value_ptr(projection_mat), glm::value_ptr(identity_mat), 100.f);

		/* 右上角方位视图 */
		/*if (!m_pEditorCamera->IsFocus())
		{
			ImGuizmo::ViewManipulate(glm::value_ptr(view_mat), m_pEditorCamera->GetDistance(), ImVec2(m_ViewportRegion.z - 128, ImGui::GetWindowPos().y), ImVec2(128, 128), 0x00000000);
			m_pEditorCamera->SetViewMatrix(view_mat);
		}*/

	}

	void SceneEditorLayer::CheckMouseSelectEntity()
	{
		PROFILE_FUNCTION();

		auto [mouse_x, mouse_y] = ImGui::GetMousePos();
		mouse_x -= m_ViewportRegion.MinX;
		mouse_y -= m_ViewportRegion.MinY;

		const glm::vec2 viewport_size = glm::vec2(m_ViewportRegion.Width, m_ViewportRegion.Height);

		if (mouse_x > 0 && mouse_y > 0 && mouse_x < viewport_size.x && mouse_y < viewport_size.y)
		{
			/* 拾取缓冲是物理像素：逻辑点 × 内容缩放后再翻到 GL 行序（纹理第 0 行在
			 * 底部，见 MetalRenderAPI::ApplyViewport），并钳到有效范围。 */
			const float content_scale = Application::Instance()->GetWindow().GetContentScale();
			const ViewportRegion& picking_region = m_pEditorCamera->GetViewportRegion();

			int pixel_data = -1;
			if (picking_region.Width > 0 && picking_region.Height > 0)
			{
				const int32_t fb_width = static_cast<int32_t>(picking_region.Width);
				const int32_t fb_height = static_cast<int32_t>(picking_region.Height);
				const int32_t pixel_x = std::clamp(static_cast<int32_t>(mouse_x * content_scale), 0, fb_width - 1);
				const int32_t pixel_y = std::clamp(
					fb_height - 1 - static_cast<int32_t>(mouse_y * content_scale), 0, fb_height - 1);

				pixel_data = m_pEditorCamera->PickingEntityByPixelPos(
					static_cast<uint32_t>(pixel_x), static_cast<uint32_t>(pixel_y));
			}

			/* 回读的是上一帧的 GPU 内容，句柄可能已失效：「句柄非空」不等于实体存在
			 * （见 Entity::operator bool 的约定），无效句柄一律当作点空。 */
			m_HoveredEntity = {};
			if (pixel_data != -1 && m_pMainScene != nullptr)
			{
				const Entity picked{ static_cast<entt::entity>(pixel_data), m_pMainScene };
				if (m_pMainScene->IsEntityValid(picked))
					m_HoveredEntity = picked;
			}

			EDITOR_LOG_DEBUG(pixel_data);
		}
	}

	void SceneEditorLayer::OnUpdate(float delta_time)
	{
		PROFILE_FUNCTION();

		if (!m_IsActivated)
			return;

		UpdateViewport();

		if (m_ViewportRegion.Width <= 0 || m_ViewportRegion.Height <= 0)
			return;

		Renderer::SetClearColor(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
		Renderer::Clear();

		if (m_pMainScene)
		{
			switch (m_PlayMode)
			{
			case PlayMode::Edit:
				/* 更新相机信息 */
				if (m_IsViewportFocused)
				{
					m_pEditorCamera->OnUpdate(delta_time);
				}

				/* 更新场景中的实体 */
				m_pMainScene->OnUpdate(delta_time, m_pEditorCamera.get());
				break;
			case PlayMode::Runtime:
				/* 运行模式下编辑器相机不响应输入，但编辑器视口仍要由它呈现 */
				m_pMainScene->OnUpdate(delta_time, m_pEditorCamera.get());
				break;
			}

			/* 执行本帧收集到的渲染视图：场景视口的内容在这里产生 */
			m_pMainScene->Render();
		}
	}
}
