#include "Pch.h"
#include "SceneEditorLayer.h"
#include <glm/gtc/type_ptr.hpp>
#include "EditorBuiltinCamera.h"
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
			//m_pFrameBuffer->Resize(m_ViewportRegion.Width(), m_ViewportRegion.Height());
			/* ImGui 给的是逻辑点，RenderTarget 必须按物理像素分配，
			 * 否则 Retina 上场景视口只渲染 1/2 分辨率再被拉伸放大。 */
			const auto content_scale = Application::Instance()->GetWindow().GetContentScale();
			const auto rt_width = static_cast<uint32_t>(m_ViewportRegion.Width * content_scale + 0.5f);
			const auto rt_height = static_cast<uint32_t>(m_ViewportRegion.Height * content_scale + 0.5f);
			m_pEditorCamera->SetViewportRegion({ 0, 0, rt_width, rt_height });
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

		const auto extension = path.extension().string();

		/* 场景文件 */
		if (extension == ".scn")
		{
			EDITOR_LOG_DEBUG("Import scene file: {}.", path.generic_string());

			SharedPtr<Scene> new_scene = CreateSharedPtr<Scene>();
			if (new_scene->Deserializer(path.generic_string()))
			{
				m_pMainScene = new_scene;
				m_ActiveScenePath = path.generic_string();

				/* 设置对象层次结构面板对应的场景 */
				m_SceneHierarchy.SetOwnerScene(m_pMainScene);

				/* 切换场景时，需更新Editor RenderView的场景 */
				m_pEditorCamera->GetRenderView()->SetOwnerScene(m_pMainScene);
			}

			return;
		}

		/* 模型文件 */
		if (extension == ".mesh")
		{
			EDITOR_LOG_DEBUG("Import model file: {}.", path.generic_string());

			Entity entity = m_pMainScene->CreateEntity(path.stem().string());
			auto& model_component = entity.AddComponent<ModelComponent>();
			model_component.m_Model = Model::Create(path.generic_string());

			return;
		}
		
	}

	void SceneEditorLayer::NewScene()
	{
		PROFILE_FUNCTION();

		m_pMainScene = CreateSharedPtr<Scene>();
		m_SceneHierarchy.SetOwnerScene(m_pMainScene);
	}

	void SceneEditorLayer::ImportScene()
	{
		PROFILE_FUNCTION();

		const auto file_path = FileDialog::OpenFile("scene(*.scn)\0*.scn\0");
		if (!file_path.empty())
		{
			m_pMainScene->Deserializer(file_path);
			m_ActiveScenePath = file_path;

			/* 设置对象层次结构面板对应的场景 */
			m_SceneHierarchy.SetOwnerScene(m_pMainScene);

			/* 切换场景时，需更新Editor RenderView的场景 */
			m_pEditorCamera->GetRenderView()->SetOwnerScene(m_pMainScene);
		}
	}

	void SceneEditorLayer::SaveScene()
	{
		PROFILE_FUNCTION();

		/* 若当前场景从未保存过，需要先确定一个保存路径 */
		if (m_ActiveScenePath.empty())
			m_ActiveScenePath = FileDialog::SaveFile("scene(*.scn)\0*.scn\0");

		m_pMainScene->Serializer(m_ActiveScenePath);
	}

	/* 保存场景到指定路径 */
	void SceneEditorLayer::SaveSceneAs()
	{
		PROFILE_FUNCTION();

		std::string file_path = FileDialog::SaveFile("scene(*.scn)\0*.scn\0");
		if (!file_path.empty())
			m_pMainScene->Serializer(file_path);

		/* 在保存之后，将当前场景路径切换为新创建的路径 */
		m_ActiveScenePath = file_path;
	}

	void SceneEditorLayer::ShowSceneViewportUI()
	{
		PROFILE_FUNCTION();

		//m_ViewportRegion = { 268, 2188, 317, 1371 };
		static ImGuiWindowFlags tab_bar_flags = ImGuiWindowFlags_NoFocusOnAppearing;

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::Begin("Scene", &m_IsActivated);
		{
			/* 获取窗口范围 */
			const auto viewport_region_min = ImGui::GetWindowContentRegionMin();
			const auto viewport_region_max = ImGui::GetWindowContentRegionMax();
			const auto viewport_offset = ImGui::GetWindowPos();
			m_ViewportRegion.MinX = viewport_region_min.x + viewport_offset.x;
			m_ViewportRegion.Width = viewport_region_max.x - viewport_region_min.x;
			m_ViewportRegion.MinY = viewport_region_min.y + viewport_offset.y;
			m_ViewportRegion.Height = viewport_region_max.y - viewport_region_min.y;

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
					const auto path = static_cast<const char *>(payload->Data);
					OnDragItemToScene(g_AssetsPath / path);
				}
				ImGui::EndDragDropTarget();
			}

			/* Gizmos */
			ShowOperationGizmoUI();
		}

		ImGui::End();
		ImGui::PopStyleVar();
	}

	void SceneEditorLayer::ShowStatisticInfoUI()
	{
		PROFILE_FUNCTION();

		ImGui::Begin("Stat Info");
		{
			if (ImGui::CollapsingHeader("GPU Stats"))
			{
				/* GPU Timer开关 */
				bool enableGPUTimer = RenderQueryProfiler::Instance().IsEnabled();
				if (ImGui::Checkbox("Enable GPU Timer", &enableGPUTimer))
					RenderQueryProfiler::Instance().SetEnabled(enableGPUTimer);

				std::function<void(const ResultGPUTimerNode&)> ShowGPUTimeResultRecursively;
				ShowGPUTimeResultRecursively = [&ShowGPUTimeResultRecursively](const ResultGPUTimerNode& timerNode)
					{
						if (timerNode.Label.empty())
							return;
						static constexpr int strLen = 40;
						char labelStr[strLen];
						snprintf(labelStr, strLen, "(%d)%s", timerNode.QueryIndex, timerNode.Label.c_str());
						labelStr[strLen - 1] = 0;

						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						if (!timerNode.Children.empty())
						{
							static auto flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth;
							const bool open = ImGui::TreeNodeEx((void*)(intptr_t)(timerNode.QueryIndex), flags, labelStr);
							ImGui::TableNextColumn();
							ImGui::Text("%.2f", timerNode.GPUTime);

							if (open)
							{
								for (const auto& child : timerNode.Children)
									ShowGPUTimeResultRecursively(child);

								ImGui::TreePop();
							}
						}
						else
						{
							static auto flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanFullWidth;
							ImGui::TreeNodeEx((void*)(intptr_t)(timerNode.QueryIndex), flags, labelStr);
							ImGui::TableNextColumn();
							ImGui::Text("%.2f", timerNode.GPUTime);
						}
					};

				static auto tableFlag = ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_NoBordersInBody;
				const auto& GPUTimerRoot = Renderer::GetGPUTimerRoot();
				if (ImGui::BeginTable("2Columes", 2, tableFlag))
				{
					ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_NoHide);
					ImGui::TableSetupColumn("GPU-Times(us)", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("A").x);
					ImGui::TableHeadersRow();
					ShowGPUTimeResultRecursively(GPUTimerRoot);
					ImGui::EndTable();
				}
			}

			// Renderer3D Stats
			if (ImGui::CollapsingHeader("3D"))
			{
				// todo: 三角形、模型数量
			}
		}
		ImGui::End();
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
		if (selected_entity && m_GizmoType != -1)
		{
			// Entity transform
			auto& transform_component = selected_entity.GetComponent<TransformComponent>();
			glm::mat4 transform_mat = transform_component.GetTransform();

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

			if (ImGuizmo::IsUsing())
			{
				/* 从变换矩阵中恢复 */
				glm::vec3 position, rotation, scale;
				DecomposeTransform(transform_mat, position, rotation, scale);

				/* 更新组件信息 */
				glm::vec3 delta_rotation = rotation - transform_component.m_Rotation;
				transform_component.m_Position = position;
				transform_component.m_Rotation += delta_rotation;
				transform_component.m_Scale = scale;
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
		mouse_y = viewport_size.y - mouse_y;

		if (mouse_x > 0 && mouse_y > 0 && mouse_x < viewport_size.x && mouse_y < viewport_size.y)
		{
			int pixel_data = m_pEditorCamera->PickingEntityByPixelPos((int)mouse_x, (int)mouse_y);
			m_HoveredEntity = pixel_data == -1 ? Entity() : Entity((entt::entity)pixel_data, m_pMainScene);
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
				/* 更新场景 */
				m_pMainScene->OnUpdate(delta_time);
				break;
			}
		}
	}
}
