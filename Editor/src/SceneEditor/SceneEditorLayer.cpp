#include "Pch.h"
#include "SceneEditorLayer.h"
#include <cstdio>
#include <glm/gtc/type_ptr.hpp>
#include "EditorBuiltinCamera.h"
#include "EditorIcons.h"
#include "SceneGizmos.h"
#include "ViewGizmo.h"
#include "PanelChrome.h"
#include "PanelRegistry.h"
#include "Command/AssetFileOps.h"
#include "Command/TransformCommand.h"
#include "Helios/Application/Application.h"
#include "Helios/Events/ApplicationEvent.h"
#include "Helios/Renderer/FrameGraph/FrameGraph.h"
#include "Helios/Scene/Components.h"
#include "Helios/Scene/ModelImporter.h"
#include "Helios/Common/PathUtils.h"
#include "Helios/Script/ScriptRuntime.h"

namespace Helios
{
	namespace
	{
		/* 顺序表草稿的键：实体句柄在高位、槽位下标在低位 */
		uint64_t ScriptOrderKey(entt::entity entity, size_t slot)
		{
			return (static_cast<uint64_t>(static_cast<uint32_t>(entity)) << 32) | static_cast<uint32_t>(slot);
		}

		/* Order 调整：Apply 一次写一个实体的全部槽位，撤销回退到写之前 */
		class ScriptOrderCommand final : public ICommand
		{
		public:
			ScriptOrderCommand(Entity entity, std::vector<int> before, std::vector<int> after)
				: m_Entity(entity), m_Before(std::move(before)), m_After(std::move(after))
			{
			}

			void Do() override { Apply(m_After); }
			void Undo() override { Apply(m_Before); }
			[[nodiscard]] const char* GetLabel() const override { return "Edit Script Order"; }

		private:
			void Apply(const std::vector<int>& orders)
			{
				if (!m_Entity || !m_Entity.HasComponent<ScriptComponent>())
					return;

				ScriptComponent& component = m_Entity.GetComponent<ScriptComponent>();
				for (size_t i = 0; i < orders.size() && i < component.m_Scripts.size(); ++i)
					component.m_Scripts[i].m_Order = orders[i];
			}

			Entity           m_Entity;
			std::vector<int> m_Before;
			std::vector<int> m_After;
		};
	}

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

	/* 模型编辑器入口：转给层级面板（Model 卡里的「Open in Model Editor」按钮用它打开模型编辑器）。 */
	void SceneEditorLayer::SetModelEditorFunc(ModelEditorFunc func)
	{
		m_SceneHierarchy.SetModelEditorFunc(std::move(func));
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

		/* 时间轴：读当前选中实体（或其最近的 Animator 祖先）的片段；
		 * 放在属性面板之后 —— 录制要看到这一帧的属性改动 */
		m_TimelinePanel.OnImGuiRender(m_pMainScene.get(), m_SceneHierarchy.GetSelectedEntity(),
			&m_CommandStack, m_IsTimelineVisible);

		/* 双击 .anim 打开：面板提交过之后再切页签，停在别的页签上的时间轴才会跳到前台 */
		if (m_FocusTimelineRequested)
		{
			m_FocusTimelineRequested = false;
			ImGui::SetWindowFocus(Panel::kTimeline);
		}

		/* 渲染图可视化页（依赖图 + 各 Pass 的中间渲染结果）：
		 * 窗口隐藏时由面板自身跳过提交（恢复入口在 View 菜单），但仍逐帧调用，
		 * 抓取请求的置位 / 清零都在面板内部结算 */
		m_FrameGraphPanel.OnImGuiRenderer(m_CameraEntries, m_IsFrameGraphVisible);

		DrawScriptOrderWindow();

		/* 主窗口，需要最后再画，以确保能够得到正确的窗口宽高 */
		ShowSceneViewportUI();

		/* 录制放在视口之后：gizmo 在同一帧里改的变换也能被抓到 */
		m_TimelinePanel.TickRecording(m_pMainScene.get(), m_SceneHierarchy.GetSelectedEntity());
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
		dispatcher.Dispatch<WindowFileDropEvent>(BIND_EVENT_FUNC(SceneEditorLayer::OnFileDrop));
	}

	void SceneEditorLayer::UpdateViewport()
	{
		PROFILE_FUNCTION();

		//const FrameBufferDescription desc = m_pFrameBuffer->GetDescription();
		if (m_ViewportRegion.Width > 0 && m_ViewportRegion.Height > 0/* && (desc.Width != m_ViewportRegion.Width() || desc.Height != m_ViewportRegion.Height())*/)
		{
			/* RenderTarget 按物理像素分配：macOS 上 ImGui 单位是点、×2 到像素（不然视口只渲染
			 * 1/2 分辨率、被拉伸）；Windows 上 ImGui 单位已是物理像素、×1（见 ImGuiToPhysicalScale）。
			 * ClampRenderSize 负责负值 / 超大值的防护。 */
			const auto rt_size = ClampRenderSize(
				static_cast<float>(m_ViewportRegion.Width),
				static_cast<float>(m_ViewportRegion.Height),
				ImGuiToPhysicalScale());
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

		/* ImGui 正收键盘（输入框激活等）时快捷键让位——否则改名框里按 Ctrl+Z 会连场景
		 * 一起撤销；消费掉，兜底层（主编辑器）也不会再响应同一批快捷键 */
		if (ImGui::GetIO().WantCaptureKeyboard)
			return true;

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
		case Key::F: /* F：视角聚焦到选中实体（指针需在视口上） */
			if (m_PlayMode == PlayMode::Edit && m_IsViewportHovered)
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
			/* 这一击真落在场景视口上才拾取：指针不在视口上（如 Frame Graph 页签
			 * 顶替视口时）、飞行 / Alt 轨道中、落在 Gizmo 或视图指示器上都不拾取 */
			if (m_IsViewportHovered && !IsGizmoHovered() && !Input::IsKeyPressed(Key::LeftAlt)
				&& !m_pEditorCamera->IsFlying() && !ImGui::IsAnyItemHovered())
			{
				CheckMouseSelectEntity();
				m_SceneHierarchy.SetSelectedEntity(m_HoveredEntity);
			}
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

	/* 响应拖拽资源到主窗口（资源浏览器内部拖拽）：.scn 打开场景，模型文件走导入 / 收录 */
	void SceneEditorLayer::OnDragItemToScene(const std::filesystem::path& path)
	{
		PROFILE_FUNCTION();

		const auto extension = PathToUtf8(path.extension());

		/* 场景文件 */
		if (extension == ".scn")
		{
			OpenScene(PathToUtf8(path));
			return;
		}

		/* 模型文件：.obj 转成 .mesh + .mtl 再进场景，.mesh 直接收录（与系统拖入同一条路） */
		ImportModelFile(path);
	}

	/* 响应系统文件拖入：指针落在场景视口上才算「拖进场景」，其余位置不处理 */
	bool SceneEditorLayer::OnFileDrop(WindowFileDropEvent* event)
	{
		PROFILE_FUNCTION();

		if (event == nullptr || !m_IsViewportVisible)
			return false;

		const float x = event->GetX();
		const float y = event->GetY();
		const auto region_x = static_cast<float>(m_ViewportRegion.MinX);
		const auto region_y = static_cast<float>(m_ViewportRegion.MinY);
		if (x < region_x || x > region_x + static_cast<float>(m_ViewportRegion.Width)
			|| y < region_y || y > region_y + static_cast<float>(m_ViewportRegion.Height))
			return false;

		bool handled = false;
		for (const std::string& path : event->GetPaths())
		{
			std::filesystem::path dropped_path;
			if (!TryPathFromUtf8(path, dropped_path))
			{
				EDITOR_LOG_WARN("Dropped file path is not valid UTF-8.");
				continue;
			}

			handled |= ImportModelFile(dropped_path);
		}
		return handled;
	}

	/* 拖入的模型文件进场景：交换格式（obj / fbx / gltf / glb / dae / stl）收进项目并转成
	 * .mesh + .mtl（已在项目里就原地转换），.mesh 直接收进项目引用；然后建实体。 */
	bool SceneEditorLayer::ImportModelFile(const std::filesystem::path& path)
	{
		PROFILE_FUNCTION();

		if (m_pMainScene == nullptr || path.empty())
			return false;

		const std::string extension = ToLowercase(PathToUtf8(path.extension()).c_str());
		const bool exchange_format = extension == ".obj" || extension == ".fbx" || extension == ".gltf"
			|| extension == ".glb" || extension == ".dae" || extension == ".stl";
		if (extension != ".mesh" && extension != ".model" && !exchange_format)
			return false;

		/* 已经在本项目资产里的（资源浏览器拖入 / 项目内的文件）原地转换；
		 * 项目外的（电脑里随便拖进来的）先收进 Assets/Models/<名字>/ */
		const std::string relative_path = AssetRelativePath(path);
		const bool in_assets = (relative_path != PathToUtf8(path));
		std::filesystem::path dest_dir = in_assets
			? path.parent_path()
			: MakeUniquePath(g_AssetsPath / "Models", PathToUtf8(path.stem()));

		std::string mesh_path;
		if (exchange_format)
		{
			EDITOR_LOG_INFO("Import model: {}.", PathToUtf8(path));
			mesh_path = ImportModel(PathToUtf8(path), PathToUtf8(dest_dir));
		}
		else
		{
			if (in_assets)
			{
				mesh_path = PathToUtf8(path);
			}
			else
			{
				/* 项目外收进来的 .mesh 只是几何 payload（白模）；带材质的模型走 .model 清单 */
				std::error_code error;
				std::filesystem::create_directories(dest_dir, error);

				const std::filesystem::path dest_mesh = dest_dir / path.filename();
				std::filesystem::copy_file(path, dest_mesh,
					std::filesystem::copy_options::overwrite_existing, error);
				if (error)
				{
					EDITOR_LOG_ERROR("Failed to copy model file: {}.", PathToUtf8(path));
					return false;
				}

				mesh_path = PathToUtf8(dest_mesh);
			}
		}

		if (mesh_path.empty())
			return false;

		return static_cast<bool>(CreateModelEntity(mesh_path));
	}

	/* 建一个挂指定 .mesh 的模型实体：拖入与导入共用，加载失败不留空实体 */
	Entity SceneEditorLayer::CreateModelEntity(const std::string& absolute_mesh_path)
	{
		PROFILE_FUNCTION();

		if (m_pMainScene == nullptr)
			return {};

		SharedPtr<Model> model = Model::Create(absolute_mesh_path);
		if (model == nullptr)
		{
			EDITOR_LOG_ERROR("Failed to load model: {}.", absolute_mesh_path);
			return {};
		}

		const std::string name = PathToUtf8(PathFromUtf8(absolute_mesh_path).stem());
		Entity entity = m_pMainScene->CreateEntity(name);
		entity.AddComponent<ModelComponent>().m_Model = std::move(model);

		/* 这条改动不走命令栈（没有对应的撤销），脏标记只能在这里显式补上 */
		m_HasUnrecordedChange = true;

		/* 新进场的模型顺手选中：属性面板里的模型信息与模型编辑器入口直接可见 */
		m_SceneHierarchy.SetSelectedEntity(entity);

		EDITOR_LOG_INFO("Place model in scene: {}.", absolute_mesh_path);
		return entity;
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

		/* 旧场景摘除编辑器相机（场景可能仍被别处引用而继续存活）；
		 * 脚本实例挂在旧场景上，一并停掉，别让它们继续引用要被替换的实体 */
		if (m_pMainScene)
		{
			m_pMainScene->GetScriptRuntime().Stop();
			m_pMainScene->RemoveExternalCamera(m_pEditorCamera.get());
		}

		m_pMainScene = scene;
		SetActiveScenePath(path);

		/* 层级面板与 RenderView 都要指向新场景；编辑器相机登记为外部相机
		 * （每帧随场景相机一起收集渲染，见 Scene::AddExternalCamera） */
		m_SceneHierarchy.SetOwnerScene(m_pMainScene);
		m_pEditorCamera->GetRenderView()->SetOwnerScene(m_pMainScene);
		m_pMainScene->AddExternalCamera(m_pEditorCamera.get());

		/* 场景内容已整体替换：缓存的实体句柄（悬停 / 选中）与旧命令都不再有效 */
		m_HoveredEntity = {};
		EndGizmoDrag();
		m_CommandStack.Clear();
		/* 顺序表的草稿指的是旧场景的槽位，一起丢掉 */
		m_ScriptOrderDraft.clear();

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

	/* 打开场景文件：加载进一个新场景，成功才切换（失败不动当前场景）。
	 * 对话框 / 拖入 / 资源浏览器双击三条入口共用这一处，行为保持一致。 */
	void SceneEditorLayer::OpenScene(const std::string& path)
	{
		PROFILE_FUNCTION();

		SharedPtr<Scene> scene = CreateSharedPtr<Scene>();
		if (!scene->Deserializer(path))
			return;

		EDITOR_LOG_INFO("Open scene: {}.", path);
		SetActiveScene(scene, path);
	}

	bool SceneEditorLayer::WantsTimelineArrowKeys() const
	{
		return m_IsTimelineVisible && m_TimelinePanel.WantsArrowKeys();
	}

	void SceneEditorLayer::OpenAnimationClip(const std::string& path)
	{
		PROFILE_FUNCTION();

		auto clip = AssetManager::Instance().GetOrLoad<AnimationClip>(path);
		if (clip == nullptr)
		{
			EDITOR_LOG_WARN("Open animation clip failed: {}.", path);
			return;
		}

		EDITOR_LOG_INFO("Edit animation clip: {}.", path);
		m_TimelinePanel.OpenClip(clip);
		m_IsTimelineVisible = true;
		m_FocusTimelineRequested = true;
	}

	void SceneEditorLayer::ImportScene()
	{
		PROFILE_FUNCTION();

		const auto file_path = FileDialog::OpenFile("scene(*.scn)\0*.scn\0");
		if (file_path.empty())
			return;

		OpenScene(file_path);
	}

	void SceneEditorLayer::SetPlayMode(PlayMode mode)
	{
		if (m_PlayMode == mode)
			return;

		m_PlayMode = mode;
		OnPlayModeChanged();
	}

	/* 脚本执行顺序总表：表格里改的是草稿，点 Apply 才写进组件（一条命令，可撤销） */
	void SceneEditorLayer::DrawScriptOrderWindow()
	{
		if (!m_IsScriptOrderVisible || m_pMainScene == nullptr)
			return;

		if (!ImGui::Begin(Panel::kScriptOrder, &m_IsScriptOrderVisible))
		{
			ImGui::End();
			return;
		}

		struct Row
		{
			Entity      Owner;
			entt::entity Handle{ entt::null };
			size_t      Slot{ 0 };
			std::string EntityName;
			std::string ClassName;
			int         Current{ 0 };   /* 组件里现在的值，行序按它排 */
			int         Draft{ 0 };     /* 表格里正在编辑的值 */
		};

		std::vector<Row> rows;
		const auto view = m_pMainScene->GetRegistry().view<ScriptComponent>();
		for (const entt::entity handle : view)
		{
			const ScriptComponent& component = view.get<ScriptComponent>(handle);
			const auto* name = m_pMainScene->GetRegistry().try_get<NameComponent>(handle);

			for (size_t i = 0; i < component.m_Scripts.size(); ++i)
			{
				Row row;
				row.Owner = Entity{ handle, m_pMainScene };
				row.Handle = handle;
				row.Slot = i;
				row.EntityName = (name != nullptr && !name->m_Name.empty()) ? name->m_Name : "Entity";
				row.ClassName = component.m_Scripts[i].ClassName;
				row.Current = component.m_Scripts[i].m_Order;

				const auto draft = m_ScriptOrderDraft.find(ScriptOrderKey(handle, i));
				row.Draft = (draft != m_ScriptOrderDraft.end()) ? draft->second : row.Current;
				rows.push_back(std::move(row));
			}
		}

		/* 行序按已经生效的值排：改完点 Apply 才重排，编辑时行不会跳来跳去 */
		std::stable_sort(rows.begin(), rows.end(),
			[](const Row& left, const Row& right) { return left.Current < right.Current; });

		ImGui::TextDisabled("Scripts run in ascending Order; equal values keep the entity and slot order.");
		if (m_PlayMode == PlayMode::Runtime)
			ImGui::TextDisabled("Order cannot be edited while running.");

		if (rows.empty())
		{
			ImGui::TextDisabled("No scripts in this scene.");
			ImGui::End();
			return;
		}

		const bool editable = (m_PlayMode != PlayMode::Runtime);
		if (ImGui::BeginTable("##ScriptOrder", 3,
			ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit))
		{
			ImGui::TableSetupColumn("Entity", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Script", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Order", ImGuiTableColumnFlags_WidthFixed, 90.0f);
			ImGui::TableHeadersRow();

			for (const Row& row : rows)
			{
				ImGui::TableNextRow();
				ImGui::PushID(static_cast<int>(static_cast<uint32_t>(row.Handle)));
				ImGui::PushID(static_cast<int>(row.Slot));

				/* 还没落盘的改动：名字标黄加星 */
				const bool pending = (row.Draft != row.Current);

				ImGui::TableNextColumn();
				if (pending)
					ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.30f, 1.0f), "%s *", row.EntityName.c_str());
				else
					ImGui::TextUnformatted(row.EntityName.c_str());

				ImGui::TableNextColumn();
				ImGui::TextUnformatted(row.ClassName.empty() ? "(none)" : row.ClassName.c_str());

				ImGui::TableNextColumn();
				int order = row.Draft;
				ImGui::SetNextItemWidth(-FLT_MIN);
				ImGui::BeginDisabled(!editable);
				if (ImGui::DragInt("##Order", &order, 1.0f))
					m_ScriptOrderDraft[ScriptOrderKey(row.Handle, row.Slot)] = order;
				ImGui::EndDisabled();

				ImGui::PopID();
				ImGui::PopID();
			}

			ImGui::EndTable();
		}

		const bool has_draft = !m_ScriptOrderDraft.empty();
		ImGui::BeginDisabled(!has_draft || !editable);
		if (ImGui::Button("Apply"))
			ApplyScriptOrderDraft();
		ImGui::SameLine();
		if (ImGui::Button("Revert"))
			m_ScriptOrderDraft.clear();
		ImGui::EndDisabled();

		ImGui::SameLine();
		ImGui::TextDisabled(has_draft ? "Unsaved order changes." : "No pending changes.");

		ImGui::End();
	}

	/* 草稿落到组件上：每个实体一条命令，撤销时整批退回 */
	void SceneEditorLayer::ApplyScriptOrderDraft()
	{
		if (m_pMainScene == nullptr || m_ScriptOrderDraft.empty())
			return;

		std::vector<entt::entity> entities;
		const auto view = m_pMainScene->GetRegistry().view<ScriptComponent>();
		for (const entt::entity handle : view)
			entities.push_back(handle);

		for (const entt::entity handle : entities)
		{
			ScriptComponent& component = m_pMainScene->GetRegistry().get<ScriptComponent>(handle);
			if (component.m_Scripts.empty())
				continue;

			std::vector<int> before;
			std::vector<int> after;
			before.reserve(component.m_Scripts.size());
			after.reserve(component.m_Scripts.size());

			bool touched = false;
			for (size_t i = 0; i < component.m_Scripts.size(); ++i)
			{
				before.push_back(component.m_Scripts[i].m_Order);

				const auto draft = m_ScriptOrderDraft.find(ScriptOrderKey(handle, i));
				const int order = (draft != m_ScriptOrderDraft.end()) ? draft->second : component.m_Scripts[i].m_Order;
				after.push_back(order);

				if (order != component.m_Scripts[i].m_Order)
					touched = true;
			}

			if (!touched)
				continue;

			m_CommandStack.Execute(CreateUniquePtr<ScriptOrderCommand>(
				Entity{ handle, m_pMainScene }, std::move(before), std::move(after)));
		}

		m_ScriptOrderDraft.clear();
	}

	/* 热重载：场景交给自己，实例的存与回灌在 ScriptRuntime 里做 */
	void SceneEditorLayer::ReloadScripts(const std::function<bool()>& swap_module)
	{
		if (m_pMainScene == nullptr)
		{
			if (swap_module)
				swap_module();
			return;
		}

		m_pMainScene->GetScriptRuntime().Reload(*m_pMainScene, swap_module);
	}

	/* 进出 Runtime：脚本实例跟着建 / 销毁。Edit 模式下场景里只留类名和参数，不跑脚本。
	 * 进出都配一次场景快照 —— 运行期间删实体、改材质都不该留在场景里。 */
	void SceneEditorLayer::OnPlayModeChanged()
	{
		if (m_pMainScene == nullptr)
			return;

		/* 顺序表的草稿跨运行时没有意义：进出一律丢掉 */
		m_ScriptOrderDraft.clear();

		if (m_PlayMode == PlayMode::Runtime)
		{
			/* 快照不写探针烘焙缓存：那是会真落盘的副作用，运行前不该有 */
			m_RuntimeSnapshotPath = std::filesystem::temp_directory_path() / "helios_runtime_snapshot.scn";
			m_pMainScene->Serializer(PathToUtf8(m_RuntimeSnapshotPath), false);

			m_pMainScene->GetScriptRuntime().Start(*m_pMainScene);
			return;
		}

		m_pMainScene->GetScriptRuntime().Stop();

		if (!m_RuntimeSnapshotPath.empty())
		{
			/* 加载是「替换」语义：实体全部重建，旧句柄一律作废 */
			if (!m_pMainScene->Deserializer(PathToUtf8(m_RuntimeSnapshotPath)))
				CORE_LOG_ERROR("Failed to restore the scene snapshot: {}", PathToUtf8(m_RuntimeSnapshotPath));

			std::error_code error;
			std::filesystem::remove(m_RuntimeSnapshotPath, error);
			m_RuntimeSnapshotPath.clear();

			/* 还原会重建实体：撤销历史与选中都指向旧句柄，一起清掉。
			 * 干净点不动 —— 进运行前本来没保存的改动，还原后仍显示为未保存。 */
			m_CommandStack.Clear();
			m_HoveredEntity = {};
			EndGizmoDrag();
			m_SceneHierarchy.SetSelectedEntity(Entity{});
		}
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
			 * 点击也会被误判成视口内选择。Gizmo 的帧状态一并清掉（本帧不再写）。 */
			m_IsViewportFocused = false;
			m_IsViewportHovered = false;
			GetTransformGizmoState() = TransformGizmoState{};
			m_GizmoHoveredHandle = -1;
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

		/* 手柄是实体网格、由渲染 Pass 画（见 SceneGizmos::SubmitTransformGizmo）：
		 * 这里只做"本帧布局 + 命中 + 拖拽"，把结果写进 GetTransformGizmoState()。 */
		TransformGizmoState& gizmo_state = GetTransformGizmoState();
		gizmo_state = TransformGizmoState{};

		Entity selected_entity = m_SceneHierarchy.GetSelectedEntity();
		/* 选择变了要先把进行中的拖拽收尾：事务窗口开着的话，下一次编辑会跟这次拖拽
		 * 被合并成同一条历史。 */
		if (selected_entity != m_GizmoEntity)
			EndGizmoDrag();

		/* 组件可能被移除；轨道旋转（Alt+LMB）期间整块跳过 —— 导航优先（跟 Godot / Blender 一致）。
		 * 缺 Transform 的话本帧就不画 Gizmo。 */
		const bool can_draw = !m_pEditorCamera->IsOrbiting() && selected_entity && m_GizmoType != -1
			&& m_pMainScene != nullptr && selected_entity.HasComponent<TransformComponent>();

		if (!can_draw)
		{
			EndGizmoDrag();
			m_GizmoLayout = {};
			m_GizmoHoveredHandle = -1;
			return;
		}

		const auto operation = static_cast<TransformGizmo::Operation>(m_GizmoType);
		auto& transform_component = selected_entity.GetComponent<TransformComponent>();

		/* Gizmo 作用在世界变换上：实体挂在父节点下时，本地变换不能直接当世界位置用。 */
		const glm::mat4 world_transform = m_pMainScene->GetWorldTransform(selected_entity);

		/* 轴基：世界变换的三列（去掉缩放）。缩放也走局部轴 —— 沿世界轴缩放会引入切变，
		 * TRS 表达不了。 */
		glm::mat3 basis(1.0f);
		for (glm::length_t axis = 0; axis < 3; ++axis)
		{
			if (!m_GizmoLocalSpace)
				continue;

			const glm::vec3 column(world_transform[axis]);
			const float length = glm::length(column);
			if (length > 1.0e-6f)
				basis[axis] = column / length;
			else
				basis[axis] = glm::vec3(axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f,
					axis == 2 ? 1.0f : 0.0f);
		}

		m_GizmoLayout = TransformGizmo::BuildLayout(glm::vec3(world_transform[3]), basis,
			m_pEditorCamera->GetPosition(), m_pEditorCamera->GetForwardDir(),
			m_pEditorCamera->GetRightDir(), m_pEditorCamera->GetUpDir(),
			ComputeGizmoScreenScale(*m_pEditorCamera),
			m_pEditorCamera->GetViewProjectionMatrix(),
			glm::vec2(static_cast<float>(m_ViewportRegion.MinX), static_cast<float>(m_ViewportRegion.MinY)),
			glm::vec2(static_cast<float>(m_ViewportRegion.Width), static_cast<float>(m_ViewportRegion.Height)));

		const glm::vec2 mouse(ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y);
		const bool mouse_in_viewport = m_IsViewportHovered
			&& mouse.x >= static_cast<float>(m_ViewportRegion.MinX)
			&& mouse.y >= static_cast<float>(m_ViewportRegion.MinY)
			&& mouse.x <= static_cast<float>(m_ViewportRegion.MinX) + static_cast<float>(m_ViewportRegion.Width)
			&& mouse.y <= static_cast<float>(m_ViewportRegion.MinY) + static_cast<float>(m_ViewportRegion.Height);

		/* 悬停：拖拽期间手柄锁定（指针移出手柄也不换），否则只在指针位于视口内时算 */
		if (!m_GizmoDrag.Active)
		{
			m_GizmoHoveredHandle = mouse_in_viewport
				? TransformGizmo::HitTest(m_GizmoLayout, operation, mouse) : -1;
		}

		/* 起手：左键按下沿 + 指针在视口上 + 没按 Alt（Alt+LMB 是轨道导航，导航优先）+
		 * 指针不在别的 ImGui 控件上（弹出的菜单、右上角视图指示器） */
		bool just_started = false;
		float gizmo_arc_sweep = 0.0f;
		float gizmo_translate_distance = 0.0f;
		float gizmo_scale_value = 0.0f;
		if (!m_GizmoDrag.Active && m_GizmoHoveredHandle >= 0
			&& ImGui::IsMouseClicked(ImGuiMouseButton_Left)
			&& mouse_in_viewport && !ImGui::IsAnyItemHovered()
			&& !Input::IsKeyPressed(Key::LeftAlt))
		{
			if (TransformGizmo::BeginDrag(m_GizmoLayout, operation, m_GizmoHoveredHandle, mouse, m_GizmoDrag))
			{
				m_GizmoEntity = selected_entity;
				m_GizmoStartTransform = transform_component;
				m_GizmoStartWorld = world_transform;
				just_started = true;

				/* 按下时开启合并窗口：整段拖拽的逐帧改动合并为一条历史 */
				m_CommandStack.BeginTransaction();
			}
		}

		if (m_GizmoDrag.Active && !just_started)
		{
			if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
			{
				EndGizmoDrag();
			}
			else
			{
				/* 拖拽期间逐帧从起点快照重算世界矩阵（不从当前值递推，长拖不会累积误差），
				 * 再按父空间换算回本地并分解 —— 挂在父节点下的实体不会随父节点一起偏移。 */
				TransformGizmo::Delta delta;
				const bool snap = Input::IsKeyPressed(Key::LeftControl);
				const bool updated = TransformGizmo::UpdateDrag(m_GizmoLayout, m_GizmoDrag, mouse, snap, delta);

				/* 角度弧画的是吸附后的角度（与落到实体上的旋转量一致） */
				if (m_GizmoDrag.Kind == TransformGizmo::Operation::Rotate)
					gizmo_arc_sweep = delta.Rotation;

				/* 缩放倍数：轴取该轴，平面取两轴之一（两轴同步），中心取整体 */
				if (m_GizmoDrag.Kind == TransformGizmo::Operation::Scale)
				{
					if (m_GizmoDrag.Handle >= 0 && m_GizmoDrag.Handle < TransformGizmo::kAxisCount)
						gizmo_scale_value = delta.Scale[m_GizmoDrag.Handle];
					else if (m_GizmoDrag.Handle >= TransformGizmo::kPlaneHandleBase)
						gizmo_scale_value = delta.Scale[
							(m_GizmoDrag.Handle - TransformGizmo::kPlaneHandleBase + 1) % TransformGizmo::kAxisCount];
					else
						gizmo_scale_value = delta.Scale.x;
				}

				/* 平移距离：轴手柄取沿轴分量（带符号），平面与中心球取位移长度 */
				if (m_GizmoDrag.Kind == TransformGizmo::Operation::Translate)
				{
					gizmo_translate_distance = m_GizmoDrag.Handle < TransformGizmo::kAxisCount
						? glm::dot(delta.Translation,
							TransformGizmo::AxisNormal(m_GizmoLayout, m_GizmoDrag.Handle))
						: glm::length(delta.Translation);
				}

				if (updated && !delta.IsIdentity())
				{
					const glm::mat4 new_world = TransformGizmo::ApplyDelta(m_GizmoDrag, delta, m_GizmoStartWorld);
					const glm::mat4 parent_world =
						m_pMainScene->GetWorldTransform(m_pMainScene->GetParent(m_GizmoEntity));

					glm::vec3 position, rotation, scale;
					if (DecomposeTransform(glm::inverse(parent_world) * new_world, position, rotation, scale))
					{
						TransformComponent next = m_GizmoStartTransform;
						next.m_Position = position;
						next.m_Rotation = rotation;
						next.m_Scale = scale;

						/* 改动经命令栈落地（同一事务内的连续改动会并成一条） */
						m_CommandStack.Execute(CreateUniquePtr<TransformCommand>(
							m_GizmoEntity, transform_component, next));
					}
				}
			}
		}

		/* 落状态给渲染侧：手柄跟着实体走（拖拽中取更新后的世界变换），高亮按按下优先。
		 * 布局退化（实体在相机背后）时不画 —— 与命中口径一致。 */
		gizmo_state.Visible = m_GizmoLayout.Valid;
		gizmo_state.Camera = m_pEditorCamera.get();
		gizmo_state.World = m_GizmoDrag.Active && m_pMainScene != nullptr
			? m_pMainScene->GetWorldTransform(m_GizmoEntity) : world_transform;
		gizmo_state.Operation = m_GizmoType;
		gizmo_state.LocalSpace = m_GizmoLocalSpace;
		gizmo_state.Highlight = m_GizmoDrag.Active ? m_GizmoDrag.Handle : m_GizmoHoveredHandle;

		/* 平移拖拽：起点到当前原点画虚线，并在屏幕上标出距离 */
		if (m_GizmoDrag.Active && m_GizmoDrag.Kind == TransformGizmo::Operation::Translate)
		{
			gizmo_state.DragActive = true;
			gizmo_state.DragStart = m_GizmoDrag.Origin;
			gizmo_state.DragCurrent = glm::vec3(gizmo_state.World[3]);
			gizmo_state.DragDistance = gizmo_translate_distance;
		}

		/* 缩放拖拽：标签显示当前倍数 */
		if (m_GizmoDrag.Active && m_GizmoDrag.Kind == TransformGizmo::Operation::Scale)
			gizmo_state.DragScale = gizmo_scale_value;

		/* 旋转拖拽中画角度弧：拖白色外环时弧落在加粗的外环上 */
		if (m_GizmoDrag.Active && m_GizmoDrag.Kind == TransformGizmo::Operation::Rotate
			&& gizmo_arc_sweep != 0.0f)
		{
			gizmo_state.ArcAxis = m_GizmoDrag.Axis;
			gizmo_state.ArcAxisIndex = m_GizmoDrag.Handle >= 0
				&& m_GizmoDrag.Handle < TransformGizmo::kAxisCount ? m_GizmoDrag.Handle : -1;
			gizmo_state.ArcStart = m_GizmoDrag.StartAngle;
			gizmo_state.ArcSweep = gizmo_arc_sweep;
			gizmo_state.ArcRadius = TransformGizmo::RingRadius(m_GizmoLayout)
				* (m_GizmoDrag.Handle == TransformGizmo::kOuterRingHandle
					? TransformGizmo::kOuterRingRatio : 1.0f);
		}

		/* 旋转角度标签：轴名 + 度数 + 弧度，画在 gizmo 中心右上方 */
		if (gizmo_state.ArcRadius > 0.0f && gizmo_state.ArcSweep != 0.0f && m_GizmoLayout.Valid)
		{
			const glm::vec4 clip = m_GizmoLayout.ViewProjection
				* glm::vec4(m_GizmoLayout.Origin, 1.0f);
			if (clip.w > 1.0e-4f)
			{
				const ImVec2 screen(
					m_GizmoLayout.ViewportMin.x
						+ (clip.x / clip.w * 0.5f + 0.5f) * m_GizmoLayout.ViewportSize.x,
					m_GizmoLayout.ViewportMin.y
						+ (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * m_GizmoLayout.ViewportSize.y);

				static const char* kAxisNames[TransformGizmo::kAxisCount] = { "X", "Y", "Z" };
				const bool axis_ring = m_GizmoDrag.Handle >= 0 && m_GizmoDrag.Handle < TransformGizmo::kAxisCount;
				const char* axis_name = axis_ring ? kAxisNames[m_GizmoDrag.Handle] : "View";

				char text[64];
				std::snprintf(text, sizeof(text), "%s : %.1f deg,  %.2f rad", axis_name,
					static_cast<double>(glm::degrees(gizmo_state.ArcSweep)),
					static_cast<double>(gizmo_state.ArcSweep));
				ImGui::GetWindowDrawList()->AddText(ImVec2(screen.x + 10.0f, screen.y - 22.0f),
					IM_COL32(255, 158, 41, 255), text);
			}
		}

		/* 缩放倍数标签：与平移、旋转用同一套字体和颜色 */
		if (gizmo_state.DragScale > 0.0f && m_GizmoLayout.Valid)
		{
			const glm::vec4 clip = m_GizmoLayout.ViewProjection
				* glm::vec4(m_GizmoLayout.Origin, 1.0f);
			if (clip.w > 1.0e-4f)
			{
				const ImVec2 screen(
					m_GizmoLayout.ViewportMin.x
						+ (clip.x / clip.w * 0.5f + 0.5f) * m_GizmoLayout.ViewportSize.x,
					m_GizmoLayout.ViewportMin.y
						+ (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * m_GizmoLayout.ViewportSize.y);

				char text[32];
				std::snprintf(text, sizeof(text), "%.2fx", static_cast<double>(gizmo_state.DragScale));
				ImGui::GetWindowDrawList()->AddText(ImVec2(screen.x + 10.0f, screen.y - 22.0f),
					IM_COL32(255, 158, 41, 255), text);
			}
		}

		/* 拖拽距离标签：起点与当前点的中点投影到屏幕，画一个小浮标（文字走 ImGui，
		 * 几何部分仍是 3D 网格） */
		if (gizmo_state.DragActive && m_GizmoLayout.Valid)
		{
			const glm::vec3 midpoint = (gizmo_state.DragStart + gizmo_state.DragCurrent) * 0.5f;
			const glm::vec4 clip = m_GizmoLayout.ViewProjection * glm::vec4(midpoint, 1.0f);
			if (clip.w > 1.0e-4f)
			{
				const ImVec2 screen(
					m_GizmoLayout.ViewportMin.x
						+ (clip.x / clip.w * 0.5f + 0.5f) * m_GizmoLayout.ViewportSize.x,
					m_GizmoLayout.ViewportMin.y
						+ (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * m_GizmoLayout.ViewportSize.y);

				char text[32];
				std::snprintf(text, sizeof(text), "%+.2f", static_cast<double>(gizmo_state.DragDistance));
				ImGui::GetWindowDrawList()->AddText(ImVec2(screen.x + 10.0f, screen.y - 22.0f),
					IM_COL32(255, 158, 41, 255), text);
			}
		}

		if (m_GizmoHoveredHandle >= 0 && !m_GizmoDrag.Active)
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	}

	void SceneEditorLayer::EndGizmoDrag()
	{
		if (!m_GizmoDrag.Active)
			return;

		m_GizmoDrag = TransformGizmo::Drag{};
		m_GizmoEntity = {};
		m_CommandStack.EndTransaction();
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

		const float kBoxSize = Scaled(92.0f);	/* 指示器方框边长（设计像素 → 物理，见 Scaled） */
		const float kBoxMargin = Scaled(6.0f);	/* 距视口上 / 右边缘 */
		const float kClickSlop = Scaled(4.0f);	/* 累计位移超过它即算拖拽（转视角），否则算点击（切视角） */
		constexpr float kQuarterTurn = glm::radians(90.0f);

		if (m_ViewportRegion.Width < static_cast<uint32_t>(kBoxSize + 2.0f * kBoxMargin) ||
			m_ViewportRegion.Height < static_cast<uint32_t>(kBoxSize + 2.0f * kBoxMargin))
			return;

		const ImVec2 box_min(
			static_cast<float>(m_ViewportRegion.MinX) + static_cast<float>(m_ViewportRegion.Width) - kBoxMargin - kBoxSize,
			static_cast<float>(m_ViewportRegion.MinY) + kBoxMargin);
		const ImVec2 center(box_min.x + kBoxSize * 0.5f, box_min.y + kBoxSize * 0.5f);
		const float orbit_radius = kBoxSize * 0.5f - ViewGizmo::kDiscRadius() - Scaled(4.0f);

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
			/* 拾取缓冲是物理像素：ImGui 坐标 × 帧缓冲换算比率后再翻到 GL 行序（纹理第 0 行在
			 * 底部，见 MetalRenderAPI::ApplyViewport），并钳到有效范围。 */
			const float to_physical = ImGuiToPhysicalScale();
			const ViewportRegion& picking_region = m_pEditorCamera->GetViewportRegion();

			int pixel_data = -1;
			if (picking_region.Width > 0 && picking_region.Height > 0)
			{
				const int32_t fb_width = static_cast<int32_t>(picking_region.Width);
				const int32_t fb_height = static_cast<int32_t>(picking_region.Height);
				const int32_t pixel_x = std::clamp(static_cast<int32_t>(mouse_x * to_physical), 0, fb_width - 1);
				const int32_t pixel_y = std::clamp(
					fb_height - 1 - static_cast<int32_t>(mouse_y * to_physical), 0, fb_height - 1);

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

			/* 拾取结果：命中打实体名 + ID，点空打 None */
			if (m_HoveredEntity && m_HoveredEntity.HasComponent<NameComponent>())
				EDITOR_LOG_DEBUG("Pick: {} (id {})",
					m_HoveredEntity.GetComponent<NameComponent>().m_Name, pixel_data);
			else
				EDITOR_LOG_DEBUG("Pick: None");
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
					m_IsViewportHovered && !IsGizmoDragging() && !ImGui::IsAnyItemHovered());
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
