#include "Pch.h"
#include "SceneEditorLayer.h"
#include <glm/gtc/type_ptr.hpp>
#include "EditorBuiltinCamera.h"
#include "EditorIcons.h"
#include "ViewGizmo.h"
#include "PanelChrome.h"
#include "PanelRegistry.h"
#include "Command/TransformCommand.h"
#include "Helios/Application/Application.h"
#include "Helios/Renderer/FrameGraph/FrameGraph.h"
#include "Helios/Scene/Components.h"
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
		/* 编辑器相机登记为外部相机：初始场景也要挂上（此后随 SetActiveScene 迁移） */
		m_pMainScene->AddExternalCamera(m_pEditorCamera.get());
		m_SceneHierarchy.SetOwnerScene(m_pMainScene);

		/* 属性面板的字段改动与撤销重做共用同一条历史 */
		m_SceneHierarchy.SetCommandStack(&m_CommandStack);






	}

	void SceneEditorLayer::OnDetached()
	{
		PROFILE_FUNCTION();

		/* 相机先于场景销毁：按登记契约摘除（编辑器相机；属性面板的预览相机随
		 * SetOwnerScene(nullptr) 一并摘除） */
		if (m_pMainScene)
			m_pMainScene->RemoveExternalCamera(m_pEditorCamera.get());

		m_SceneHierarchy.SetOwnerScene(nullptr);

		ILayer::OnDetached();
	}

	/* 资源定位通道：转给层级面板（材质卡里的贴图点击用它定位资源浏览器）。 */
	void SceneEditorLayer::SetAssetRevealFunc(AssetRevealFunc func)
	{
		m_SceneHierarchy.SetAssetRevealFunc(std::move(func));
	}

	/* 资源选中通道：转给层级面板（属性面板显示资源的详细内容）。 */
	void SceneEditorLayer::SetAssetSelection(const std::vector<AssetSelectionEntry>& selection)
	{
		m_SceneHierarchy.SetAssetSelection(selection);
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

		/* 渲染图可视化页（依赖图 + 各 Pass 的中间渲染结果）：
		 * 窗口隐藏时由面板自身跳过提交（恢复入口在 View 菜单），但仍逐帧调用，
		 * 抓取请求的置位 / 清零都在面板内部结算 */
		m_FrameGraphPanel.OnImGuiRenderer(m_CameraEntries, m_IsFrameGraphVisible);

		/* 主窗口，需要最后再画，以确保能够得到正确的窗口宽高 */
		ShowSceneViewportUI();
	}

	void SceneEditorLayer::OnEvent(IEvent* event)
	{
		PROFILE_FUNCTION();

		if (!m_IsActivated || !event)
			return;

		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<KeyPressedEvent>(BIND_EVENT_FUNC(SceneEditorLayer::OnKeyPressed));
		dispatcher.Dispatch<MouseButtonPressedEvent>(BIND_EVENT_FUNC(SceneEditorLayer::OnMouseButtonPressed));
		dispatcher.Dispatch<MouseScrolledEvent>(BIND_EVENT_FUNC(SceneEditorLayer::OnMouseScrolled));
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

	/* 场景相机渲染到窗口默认目标：视口区域 = 窗口画布（物理像素）。引擎样例由宿主设置（见
	 * Samples::SetViewportRegion）；不设置就恒为 0×0（ScenePass 空跑）。逐帧写，窗口缩放 / 换场景
	 * 都会自动跟上。 */
	void SceneEditorLayer::UpdateSceneCameraViewports()
	{
		PROFILE_FUNCTION();

		if (!m_pMainScene)
			return;

		const auto& window = Application::Instance()->GetWindow();
		const uint32_t width = window.GetWidth();
		const uint32_t height = window.GetHeight();
		if (width == 0 || height == 0)
			return; /* 最小化等异常尺寸：区域要求 > 0（SetViewportRegion 有断言） */

		entt::registry& registry = m_pMainScene->GetRegistry();
		const auto camera_view = registry.view<CameraComponent>();
		for (auto entity : camera_view)
		{
			const auto& camera_component = camera_view.get<CameraComponent>(entity);
			if (camera_component.m_Camera != nullptr)
			{
				camera_component.m_Camera->GetRenderView()->SetViewportRegion(
					{ 0, 0, width, height });
			}
		}
	}

	/* 收集本帧可视化可选的相机：编辑器相机在前、场景相机（按实体名）随后。
	 * 渲染图与相机同生命周期、指针跨帧稳定（面板选中匹配与抓取目标都按它）；
	 * 场景相机的视图每帧由 Scene 收集执行、编辑器相机经外部相机登记 —— 两者都有渲染图。 */
	void SceneEditorLayer::CollectCameraEntries()
	{
		m_CameraEntries.clear();
		m_CameraEntries.push_back({ "Editor Camera", m_pEditorCamera->GetRenderView()->GetFrameGraph().get() });

		if (!m_pMainScene)
			return;

		entt::registry& registry = m_pMainScene->GetRegistry();
		const auto camera_view = registry.view<CameraComponent>();
		for (auto entity : camera_view)
		{
			const auto& camera_component = camera_view.get<CameraComponent>(entity);
			if (camera_component.m_Camera == nullptr)
				continue;

			std::string camera_name = "Camera";
			if (const auto* name_component = registry.try_get<NameComponent>(entity))
				camera_name = name_component->m_Name;

			m_CameraEntries.push_back({ std::move(camera_name),
				camera_component.m_Camera->GetRenderView()->GetFrameGraph().get() });
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
		case Key::F: /* F：视角聚焦到选中实体（指针需在视口上，且在输入文本时让位） */
			if (m_PlayMode == PlayMode::Edit && m_IsViewportHovered && !ImGui::GetIO().WantCaptureKeyboard)
				FocusSelectedEntity();
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

			/* 飞行中（按住 RMB）左键不作选择：避免飞行操作误改变选中；
			 * 指针落在视口内控件（视图指示器）上时点击也不穿透到场景 */
			if (m_IsViewportHovered && !ImGuizmo::IsOver() && !Input::IsKeyPressed(Key::LeftAlt)
				&& !m_pEditorCamera->IsFlying() && !ImGui::IsAnyItemHovered())
				m_SceneHierarchy.SetSelectedEntity(m_HoveredEntity);
		}
		return false;
	}

	/* 滚轮：飞行中调移动速度，其余情况拉近拉远。只在指针位于视口上时接管，
	 * 其余窗口的滚轮留给 ImGui 自己处理。 */
	bool SceneEditorLayer::OnMouseScrolled(MouseScrolledEvent* event)
	{
		PROFILE_FUNCTION();

		if (m_PlayMode != PlayMode::Edit || !m_IsViewportHovered)
			return false;

		const float wheel = event->GetYOffset();
		if (wheel == 0.0f)
			return false;

		if (m_pEditorCamera->IsFlying())
			m_pEditorCamera->AdjustMoveSpeed(wheel);
		else
			m_pEditorCamera->OnMouseWheelZoom(wheel);

		return true;
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

		/* 旧场景摘除编辑器相机（场景可能仍被别处引用而继续存活） */
		if (m_pMainScene)
			m_pMainScene->RemoveExternalCamera(m_pEditorCamera.get());

		m_pMainScene = scene;
		SetActiveScenePath(path);

		/* 层级面板与 RenderView 都要指向新场景；编辑器相机登记为外部相机
		 * （每帧随场景相机一起收集渲染，见 Scene::AddExternalCamera） */
		m_SceneHierarchy.SetOwnerScene(m_pMainScene);
		m_pEditorCamera->GetRenderView()->SetOwnerScene(m_pMainScene);
		m_pMainScene->AddExternalCamera(m_pEditorCamera.get());

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

			/* 右上角视图指示器（点击 / 拖拽切换与旋转视角） */
			ShowViewGizmoUI();

			/* Gizmos */
			ShowOperationGizmoUI();
		}
		else
		{
			/* 视口窗口不可见：清掉交互标志，否则残留的 hover 会继续给相机授权、
			 * 点击也会被误判成视口内选择。 */
			m_IsViewportFocused = false;
			m_IsViewportHovered = false;
		}

		ImGui::End();
		ImGui::PopStyleVar();
	}

	void SceneEditorLayer::ShowStatisticInfoUI()
	{
		PROFILE_FUNCTION();

		/* 绘制逻辑在面板自身（每帧 CPU / GPU 耗时 + GPU 计时树；改动必跑 UI 冒烟检查） */
		m_RenderStatsPanel.OnImGuiRender();
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
		/* 组件可能被移除，缺 Transform 的话本帧就不画 Gizmo。轨道旋转（Alt+LMB）期间整块跳过：
		 * ImGuizmo 只认左键，不跳过的话 Alt+LMB 起手落到轴上会同时拖动物体 —— 导航优先（跟 Godot /
		 * Blender 一致）。 */
		if (!m_pEditorCamera->IsOrbiting() && selected_entity && m_GizmoType != -1
			&& m_pMainScene && selected_entity.HasComponent<TransformComponent>())
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

	}

	/* 右上角视图指示器：六个轴盘（X / Y / Z × ±）—— 点击切视角、拖拽轨道旋转。盘面样式和
	 * 几何 / 命中算法在 ViewGizmo.h（位置 = 世界轴经视线旋转投影到屏幕、深度决定压盖顺序）；
	 * 本函数只管交互。 */
	void SceneEditorLayer::ShowViewGizmoUI()
	{
		PROFILE_FUNCTION();

		/* 运行模式下编辑器相机不响应输入，指示器一并停用 */
		if (m_PlayMode != PlayMode::Edit)
			return;

		constexpr float kBoxSize = 92.0f;		/* 指示器方框边长（逻辑像素） */
		constexpr float kBoxMargin = 6.0f;		/* 距视口上 / 右边缘 */
		constexpr float kClickSlop = 4.0f;		/* 累计位移超过它即算拖拽（转视角），否则算点击（切视角） */
		constexpr float kQuarterTurn = glm::radians(90.0f);

		if (m_ViewportRegion.Width < static_cast<uint32_t>(kBoxSize + 2.0f * kBoxMargin) ||
			m_ViewportRegion.Height < static_cast<uint32_t>(kBoxSize + 2.0f * kBoxMargin))
			return;

		const ImVec2 box_min(
			static_cast<float>(m_ViewportRegion.MinX) + static_cast<float>(m_ViewportRegion.Width) - kBoxMargin - kBoxSize,
			static_cast<float>(m_ViewportRegion.MinY) + kBoxMargin);
		const ImVec2 center(box_min.x + kBoxSize * 0.5f, box_min.y + kBoxSize * 0.5f);
		const float orbit_radius = kBoxSize * 0.5f - ViewGizmo::kDiscRadius - 4.0f;

		/* 六个轴盘：世界轴方向经视线旋转投影 + 深度排序（近者在后 = 后画、优先命中） */
		ViewGizmo::Disc discs[ViewGizmo::kDiscCount];
		ViewGizmo::ComputeDiscs(glm::mat3(m_pEditorCamera->GetViewMatrix()), center, orbit_radius, discs);

		ImGui::SetCursorScreenPos(box_min);
		ImGui::InvisibleButton("##ViewGizmo", ImVec2(kBoxSize, kBoxSize));

		/* 命中：指针落在轴盘圆内，从近到远取第一个（近者优先） */
		const int32_t hovered_disc = ImGui::IsItemHovered()
			? ViewGizmo::HitTest(discs, ImGui::GetIO().MousePos) : -1;

		/* 按下沿记录命中（供"点击 = 切视角"判定）；拖过阈值即转为轨道旋转 */
		if (ImGui::IsItemActivated())
		{
			m_ViewGizmoPressedDisc = hovered_disc;
			m_ViewGizmoDraggedDistance = 0.0f;
		}

		if (ImGui::IsItemActive())
		{
			const ImVec2 drag_delta = ImGui::GetIO().MouseDelta;
			m_ViewGizmoDraggedDistance += std::fabs(drag_delta.x) + std::fabs(drag_delta.y);
			if (m_ViewGizmoDraggedDistance > kClickSlop)
				m_ViewGizmoPressedDisc = -1;

			if (drag_delta.x != 0.0f || drag_delta.y != 0.0f)
				m_pEditorCamera->OrbitByPixelDelta(glm::vec2(drag_delta.x, drag_delta.y));
		}

		if (ImGui::IsItemDeactivated())
		{
			/* 点击（未拖拽）落在轴盘上：切换到该视角；+Y / -Y 保留当前方位角 */
			switch (m_ViewGizmoPressedDisc)
			{
			case 0: m_pEditorCamera->SetOrbitAngles(-kQuarterTurn, 0.0f); break;	/* 从 +X 看 */
			case 1: m_pEditorCamera->SetOrbitAngles(kQuarterTurn, 0.0f); break;		/* 从 -X 看 */
			case 2: m_pEditorCamera->SetOrbitAngles(m_pEditorCamera->GetYaw(), kQuarterTurn); break;	/* 俯视 */
			case 3: m_pEditorCamera->SetOrbitAngles(m_pEditorCamera->GetYaw(), -kQuarterTurn); break;	/* 仰视 */
			case 4: m_pEditorCamera->SetOrbitAngles(0.0f, 0.0f); break;				/* 从 +Z 看 */
			case 5: m_pEditorCamera->SetOrbitAngles(glm::radians(180.0f), 0.0f); break;	/* 从 -Z 看 */
			default: break;
			}
			m_ViewGizmoPressedDisc = -1;
		}

		/* 绘制：由远及近；正轴实心 / 负轴空心 + 字母，悬停 / 按下高亮一档。
		 * 拖拽（轨道）进行中不给悬停高亮：轴盘在指针下穿行，逐帧换高亮会闪成一片；
		 * 按下沿的高亮（m_ViewGizmoPressedDisc）不受影响，点击语义仍看得见 */
		const int32_t highlight_disc = ImGui::IsItemActive() ? -1 : hovered_disc;
		ViewGizmo::Draw(ImGui::GetWindowDrawList(), discs, highlight_disc, m_ViewGizmoPressedDisc);

		if (hovered_disc >= 0)
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
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

	/* F：把轨道中心聚焦到选中实体，并按包围球取景 */
	void SceneEditorLayer::FocusSelectedEntity()
	{
		PROFILE_FUNCTION();

		Entity selected_entity = m_SceneHierarchy.GetSelectedEntity();
		if (!selected_entity || m_pMainScene == nullptr || !selected_entity.HasComponent<TransformComponent>())
			return;

		const glm::mat4 world_transform = m_pMainScene->GetWorldTransform(selected_entity);
		glm::vec3 focus_center = glm::vec3(world_transform[3]);
		float focus_radius = 1.0f;

		if (selected_entity.HasComponent<ModelComponent>())
		{
			const auto& model = selected_entity.GetComponent<ModelComponent>().m_Model;
			if (model != nullptr)
			{
				/* 模型局部 AABB → 世界：中心随变换走，半径按世界缩放折算（包围球近似） */
				const glm::vec3 local_center = (model->GetAABBMin() + model->GetAABBMax()) * 0.5f;
				const glm::vec3 local_extents = (model->GetAABBMax() - model->GetAABBMin()) * 0.5f;
				focus_center = glm::vec3(world_transform * glm::vec4(local_center, 1.0f));

				const glm::vec3 world_scale{
					glm::length(glm::vec3(world_transform[0])),
					glm::length(glm::vec3(world_transform[1])),
					glm::length(glm::vec3(world_transform[2])) };
				focus_radius = glm::length(local_extents * world_scale);
			}
		}

		m_pEditorCamera->FocusOn(focus_center, glm::max(focus_radius, 0.1f));
	}

	void SceneEditorLayer::OnUpdate(float delta_time)
	{
		PROFILE_FUNCTION();

		if (!m_IsActivated)
			return;

		UpdateViewport();
		UpdateSceneCameraViewports();

		/* 相机列表与面板 UI 同源：即使本帧不渲染（视口为 0）也刷新，面板不读过期条目 */
		CollectCameraEntries();

		if (m_ViewportRegion.Width <= 0 || m_ViewportRegion.Height <= 0)
			return;

		Renderer::SetClearColor(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
		Renderer::Clear();

		if (m_pMainScene)
		{
			switch (m_PlayMode)
			{
			case PlayMode::Edit:
				/* 更新相机信息：指针悬停在视口上才授权起手导航（已起手的手势不受影响）；
				 * Gizmo 拖拽中、指针落在视口内控件（视图指示器）上时都不让出输入 */
				m_pEditorCamera->SetNavigationAllowed(
					m_IsViewportHovered && !ImGuizmo::IsUsing() && !ImGui::IsAnyItemHovered());
				m_pEditorCamera->OnUpdate(delta_time);

				/* 更新场景中的实体 */
				m_pMainScene->OnUpdate(delta_time);
				break;
			case PlayMode::Runtime:
				/* 运行模式下编辑器相机不响应输入，但编辑器视口仍要由它呈现 */
				m_pMainScene->OnUpdate(delta_time);
				break;
			}

			/* Frame Graph 面板的抓取目标（上一帧 UI 落下）：面板可见且开着 Capture 时，
			 * 只给所选相机的渲染图打开抓取（其余一律关）—— 本帧渲染图的执行会抓取
			 * 各 Pass 的中间渲染结果（面板关闭时抓取零开销） */
			{
				const bool capture_open = m_FrameGraphPanel.IsCaptureRequested();
				const FrameGraph* capture_target = m_FrameGraphPanel.GetCaptureTarget();
				for (const auto& camera_entry : m_CameraEntries)
				{
					if (camera_entry.Graph != nullptr)
						camera_entry.Graph->GetCapture().SetEnabled(capture_open && camera_entry.Graph == capture_target);
				}
			}

			/* 执行本帧收集到的渲染视图：场景视口的内容在这里产生 */
			m_pMainScene->Render();
		}
	}
}
