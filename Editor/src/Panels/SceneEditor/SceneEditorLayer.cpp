#include "Pch.h"
#include "SceneEditorLayer.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <glm/gtc/type_ptr.hpp>
#include "Viewport/EditorBuiltinCamera.h"
#include "UI/EditorIcons.h"
#include "Viewport/SceneGizmos.h"
#include "Viewport/ViewGizmo.h"
#include "Viewport/TerrainBrushPreview.h"
#include "UI/PanelChrome.h"
#include "UI/PanelRegistry.h"
#include "Panels/PanelWindow.h"
#include "Command/AssetFileOps.h"
#include "Command/TransformCommand.h"
#include "Command/TerrainSculptCommand.h"
#include "Panels/SceneEditor/TerrainSculpt.h"
#include "Helios/Application/Application.h"
#include "Helios/Events/ApplicationEvent.h"
#include "Helios/Renderer/FrameGraph/FrameGraph.h"
#include "Helios/Scene/Components.h"
#include "Assets/ModelImporter.h"
#include "Common/FileDialog.h"
#include "Helios/Common/PathUtils.h"
#include "Helios/Scene/TerrainField.h"
#include "Helios/Scene/GrassField.h"
#include "Common/ProjectConfig.h"
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

		class GrassPaintCommand final : public ICommand
		{
		public:
			GrassPaintCommand(Entity entity, glm::uvec2 min, glm::uvec2 max,
				std::vector<uint8_t> before, std::vector<uint8_t> after, bool empty_before)
				: m_Entity(entity), m_Min(min), m_Max(max), m_Before(std::move(before)),
				  m_After(std::move(after)), m_EmptyBefore(empty_before) {}
			void Do() override { Apply(m_After, false); }
			void Undo() override { Apply(m_Before, m_EmptyBefore); }
			[[nodiscard]] const char* GetLabel() const override { return "Paint Grass Density"; }
		private:
			void Apply(const std::vector<uint8_t>& values, bool clear)
			{
				if (!m_Entity || !m_Entity.HasComponent<GrassComponent>())
					return;
				auto& grass = m_Entity.GetComponent<GrassComponent>();
				if (clear)
					grass.m_DensityMask.clear();
				else
				{
					constexpr uint32_t n = GrassComponent::kDensityMaskSize;
					if (grass.m_DensityMask.empty())
						grass.m_DensityMask.assign(size_t(n) * n, 255u);
					if (grass.m_DensityMask.size() != size_t(n) * n)
						return;
					size_t index = 0;
					for (uint32_t z = m_Min.y; z <= m_Max.y; ++z)
						for (uint32_t x = m_Min.x; x <= m_Max.x; ++x)
							grass.m_DensityMask[size_t(z) * n + x] = values[index++];
				}
				++grass.m_DensityMaskRevision;
			}
			Entity m_Entity;
			glm::uvec2 m_Min, m_Max;
			std::vector<uint8_t> m_Before, m_After;
			bool m_EmptyBefore;
		};

		/* 视角记录文件：跟层级树的展开状态一样放在项目的 .helios 下 */
		std::filesystem::path ViewStatePath()
		{
			return g_ProjectPath / ".helios" / "view.cfg";
		}

		/* 场景的键取相对项目根的路径：对话框给绝对路径、资源浏览器给相对路径，
		 * 两种写法要落到同一把钥匙，否则同一个场景会攒出两份视角 */
		std::string ViewKey(const std::string& path)
		{
			return PathToUtf8(ToAssetRelative(PathFromUtf8(path)));
		}

		/* 视角停手多久后写盘：关窗走 OnDetached，被系统退出就不一定了 */
		constexpr float kViewAutosaveSeconds = 1.5f;
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
		/* 层级树的展开状态跟着项目走：<项目>/.helios/hierarchy.cfg，按场景分段 */
		m_SceneHierarchy.SetTreeStatePath(PathToUtf8(g_ProjectPath / ".helios" / "hierarchy.cfg"));
		/* 各场景的视角：启动场景打开时要用，先读进来 */
		LoadViewStates();

		/* 属性面板的字段改动与撤销重做共用同一条历史 */
		m_SceneHierarchy.SetCommandStack(&m_CommandStack);

		/* 层级树双击 = 镜头定位到该实体（与 F 键同一套取景） */
		m_SceneHierarchy.SetFocusEntityFunc([this](const Entity& entity) { FocusEntity(entity); });

		/* 启动就开一个场景：上次打开的那个，没有才退回项目配的启动场景 */
		OpenStartupScene();
	}

	/* 上次打开的场景优先，其次是项目配的启动场景：文件被删或移走就顺位往下退 */
	void SceneEditorLayer::OpenStartupScene()
	{
		PROFILE_FUNCTION();

		const ProjectConfig config = LoadProjectConfig();

		std::vector<std::string> candidates;
		if (!m_LastScenePath.empty())
			candidates.push_back(m_LastScenePath);
		if (!config.StartupScene.empty())
			candidates.push_back(config.StartupScene);

		for (const std::string& candidate : candidates)
		{
			const std::filesystem::path scene_path = ResolveAssetPath(PathFromUtf8(candidate));
			if (std::filesystem::is_regular_file(scene_path))
			{
				OpenScene(PathToUtf8(scene_path));
				return;
			}

			EDITOR_LOG_WARN("Scene not found, skipped: {}.", candidate);
		}
	}

	void SceneEditorLayer::OnDetached()
	{
		PROFILE_FUNCTION();

		/* 关编辑器前把当前场景的视角落盘，下次打开还是这一眼 */
		CaptureViewState();
		StoreViewStates();

		/* 相机先于场景销毁：按登记契约摘除（编辑器相机；属性面板的预览相机随
		 * SetOwnerScene(nullptr) 一并摘除） */
		if (m_pMainScene)
			m_pMainScene->RemoveExternalCamera(m_pEditorCamera.get());

		m_SceneHierarchy.SetOwnerScene(nullptr);

		ILayer::OnDetached();
	}

	/* 资源定位通道：转给属性面板（材质卡里的贴图点击用它定位资源浏览器）。 */
	void SceneEditorLayer::SetAssetRevealFunc(AssetRevealFunc func)
	{
		m_SceneHierarchy.GetInspector().SetAssetRevealFunc(std::move(func));
	}

	/* 资源选中通道：转给属性面板（显示资源的详细内容）。 */
	void SceneEditorLayer::SetAssetSelection(const std::vector<AssetSelectionEntry>& selection)
	{
		m_SceneHierarchy.GetInspector().SetAssetSelection(selection);
	}

	/* 模型编辑器入口：转给属性面板（Model 卡里的「Open in Model Editor」按钮用它打开模型编辑器）。 */
	void SceneEditorLayer::SetModelEditorFunc(ModelEditorFunc func)
	{
		m_SceneHierarchy.GetInspector().SetModelEditorFunc(std::move(func));
	}

	void SceneEditorLayer::OnImGuiRender()
	{
		PROFILE_FUNCTION();

		/* 统计信息 */
		ShowStatisticInfoUI();

		/* 脏标记每帧推送：场景名旁的圆点必须和编辑历史同步，不能缓存成独立状态 */
		m_SceneHierarchy.SetSceneDirty(IsSceneDirty());

		/* 场景管理及属性窗口 */
		m_SceneHierarchy.OnImGuiRender();

		/* 时间轴：读当前选中实体（或其最近的 Animator 祖先）的片段；
		 * 放在属性面板之后 —— 录制要看到这一帧的属性改动 */
		m_TimelinePanel.OnImGuiRender(m_pMainScene.get(), m_SceneHierarchy.GetSelectedEntity(),
			&m_CommandStack, Panel::VisibleFlag(Panel::kTimeline));

		/* 双击 .anim 打开：面板提交过之后再切页签，停在别的页签上的时间轴才会跳到前台 */
		if (m_FocusTimelineRequested)
		{
			m_FocusTimelineRequested = false;
			ImGui::SetWindowFocus(Panel::kTimeline);
		}

		/* 渲染图可视化页（依赖图 + 各 Pass 的中间渲染结果）：
		 * 窗口隐藏时由面板自身跳过提交（恢复入口在 View 菜单），但仍逐帧调用，
		 * 抓取请求的置位 / 清零都在面板内部结算 */
		m_FrameGraphPanel.OnImGuiRenderer(m_CameraEntries, Panel::VisibleFlag(Panel::kFrameGraph));

		DrawScriptOrderWindow();

		/* 主窗口，需要最后再画，以确保能够得到正确的窗口宽高 */
		ShowSceneViewportUI();

		/* 录制放在视口之后：gizmo 在同一帧里改的变换也能被抓到 */
		m_TimelinePanel.TickRecording(m_pMainScene.get(), m_SceneHierarchy.GetSelectedEntity());
	}

	void SceneEditorLayer::OnEvent(IEvent* event)
	{
		PROFILE_FUNCTION();

		if (!event)
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

	/* 场景相机的画面尺寸 = 窗口画布（物理像素），与运行时一致。引擎样例由宿主设置（见
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

	void SceneEditorLayer::UpdateSceneCameraRendering()
	{
		PROFILE_FUNCTION();

		if (!m_pMainScene)
			return;

		/* 运行模式照常渲染，统计面板能看到游戏相机的真实开销 */
		const bool runtime = (m_PlayMode == PlayMode::Runtime);
		const FrameGraph* viewed_graph = m_FrameGraphPanel.GetViewedGraph();
		const Camera* preview_camera = m_SceneHierarchy.GetInspector().GetPreviewCamera();

		entt::registry& registry = m_pMainScene->GetRegistry();
		const auto camera_view = registry.view<CameraComponent>();
		for (auto entity : camera_view)
		{
			const auto& camera_component = camera_view.get<CameraComponent>(entity);
			if (camera_component.m_Camera == nullptr)
				continue;

			RenderView* render_view = camera_component.m_Camera->GetRenderView();
			const bool viewed = render_view->GetFrameGraph().get() == viewed_graph
				|| camera_component.m_Camera.get() == preview_camera;
			render_view->SetEnableRender(runtime || viewed);
			render_view->SetOutputToTexture(true);
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
			/* 笔刷模式下、选中的是地形时左键是用来画的，不改选中；选中别的照常点选 */
			const Entity selected = m_SceneHierarchy.GetSelectedEntity();
			if (m_IsViewportHovered && selected && !IsGizmoHovered()
				&& !Input::IsKeyPressed(Key::LeftAlt) && !ImGui::IsAnyItemHovered()
				&& ((TerrainSculpt::GetSettings().Brush != TerrainSculpt::Mode::None
					&& selected.HasComponent<TerrainComponent>())
					|| (GrassPaint::GetSettings().Brush != GrassPaint::Mode::None
						&& selected.HasComponent<GrassComponent>())))
				return false;

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

		if (event == nullptr || !Panel::IsVisible(Panel::kScene))
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

		const std::string extension = LowercaseExtension(path);
		const bool exchange_format = extension == ".obj" || extension == ".fbx" || extension == ".gltf"
			|| extension == ".glb" || extension == ".dae" || extension == ".stl";
		if (extension != ".mesh" && extension != ".model" && !exchange_format)
			return false;

		/* 已经在本项目资产里的（资源浏览器拖入 / 项目内的文件）原地转换；
		 * 项目外的（电脑里随便拖进来的）先收进项目 Models/<名字>/ */
		const std::string relative_path = AssetRelativePath(path);
		const bool in_assets = (relative_path != PathToUtf8(path));
		std::filesystem::path dest_dir = in_assets
			? path.parent_path()
			: MakeUniquePath(g_ProjectPath / "Models", PathToUtf8(path.stem()));

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
		m_ActiveSceneKey = path.empty() ? std::string() : ViewKey(path);
		/* 记下这次打开的是谁：下次启动直接接上；新建还没保存的场景就是空的 */
		if (m_LastScenePath != m_ActiveSceneKey)
		{
			m_LastScenePath = m_ActiveSceneKey;
			m_ViewStatesDirty = true;
		}

		/* 层级面板的根节点就是当前场景名：路径改了必须同步，否则另存为之后显示的仍是旧名字 */
		m_SceneHierarchy.SetScenePath(path);

		/* 换场景就把笔刷关掉：工具状态是编辑器级的全局量，留着的话新场景里一按左键就开始画 */
		TerrainSculpt::GetSettings().Brush = TerrainSculpt::Mode::None;
		GrassPaint::GetSettings().Brush = GrassPaint::Mode::None;
		TerrainSculpt::GetExportRequest() = {};
		m_GrassPaintActive = false;
		m_GrassPaintTarget = {};
		m_GrassMaskBefore.clear();
		m_SculptActive = false;
		m_SculptTerrain = {};
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

	/* 文件格式（<项目>/.helios/view.cfg）：
	 *   LastScene=上次打开的场景
	 *   [场景路径]
	 *   Pivot=x y z / Angles=yaw pitch / Distance=d */
	void SceneEditorLayer::LoadViewStates()
	{
		PROFILE_FUNCTION();

		if (m_ViewStatesLoaded)
			return;

		m_ViewStatesLoaded = true;

		std::ifstream stream(ViewStatePath());
		if (!stream)
			return;

		/* 认不出的键直接跳过：旧版本的文件照样能读 */
		std::string scene;
		for (std::string line; std::getline(stream, line);)
		{
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (line.size() < 3)
				continue;

			if (line.front() == '[' && line.back() == ']')
			{
				scene = line.substr(1, line.size() - 2);
				continue;
			}

			const size_t separator = line.find('=');
			if (separator == std::string::npos)
				continue;

			const std::string key = line.substr(0, separator);
			const std::string raw = line.substr(separator + 1);

			/* 分段外面这一行记的是「上次打开的场景」 */
			if (key == "LastScene")
			{
				m_LastScenePath = raw;
				continue;
			}

			if (scene.empty())
				continue;

			SceneViewState& state = m_ViewStates[scene];
			if (key == "Pivot")
				std::sscanf(raw.c_str(), "%f %f %f", &state.Pivot.x, &state.Pivot.y, &state.Pivot.z);
			else if (key == "Angles")
				std::sscanf(raw.c_str(), "%f %f", &state.Yaw, &state.Pitch);
			else if (key == "Distance")
				state.Distance = static_cast<float>(std::atof(raw.c_str()));
		}
	}

	void SceneEditorLayer::StoreViewStates()
	{
		PROFILE_FUNCTION();

		if (!m_ViewStatesDirty)
			return;

		m_ViewStatesDirty = false;

		std::error_code error;
		std::filesystem::create_directories(ViewStatePath().parent_path(), error);

		std::ofstream stream(ViewStatePath(), std::ios::trunc);
		if (!stream)
			return;

		stream << std::fixed << std::setprecision(4);

		/* 空着就不写这行：下次启动直接落到项目的启动场景 */
		if (!m_LastScenePath.empty())
			stream << "LastScene=" << m_LastScenePath << "\n";

		for (const auto& [scene, state] : m_ViewStates)
		{
			stream << "[" << scene << "]\n";
			stream << "Pivot=" << state.Pivot.x << " " << state.Pivot.y << " " << state.Pivot.z << "\n";
			stream << "Angles=" << state.Yaw << " " << state.Pitch << "\n";
			stream << "Distance=" << state.Distance << "\n";
		}
	}

	void SceneEditorLayer::CaptureViewState()
	{
		PROFILE_FUNCTION();

		/* 新建还没保存的场景没有名字，没处记 */
		if (m_ActiveSceneKey.empty() || m_pEditorCamera == nullptr)
			return;

		SceneViewState& state = m_ViewStates[m_ActiveSceneKey];
		state.Pivot = m_pEditorCamera->GetFocalPoint();
		state.Yaw = m_pEditorCamera->GetYaw();
		state.Pitch = m_pEditorCamera->GetPitch();
		state.Distance = m_pEditorCamera->GetDistance();
		m_ViewStatesDirty = true;
	}

	void SceneEditorLayer::ApplyViewState()
	{
		PROFILE_FUNCTION();

		/* 新建还没保存的场景没有名字，也就没有记录可找 */
		if (m_ActiveSceneKey.empty())
			return;

		const auto found = m_ViewStates.find(m_ActiveSceneKey);
		if (found == m_ViewStates.end())
			return;

		const SceneViewState& state = found->second;
		m_pEditorCamera->SetFocalPoint(state.Pivot);
		m_pEditorCamera->SetOrbitAngles(state.Yaw, state.Pitch);
		m_pEditorCamera->SetDistance(state.Distance);
	}

	/* 视角动了就重新计时，停手够久才写一次盘：拖拽中不反复写文件，
	 * 被系统直接退出也不至于丢掉这一段导航 */
	void SceneEditorLayer::UpdateViewStateAutosave(float delta_time)
	{
		PROFILE_FUNCTION();

		if (m_ActiveSceneKey.empty() || m_pEditorCamera == nullptr)
			return;

		const SceneViewState live{ m_pEditorCamera->GetFocalPoint(), m_pEditorCamera->GetYaw(),
			m_pEditorCamera->GetPitch(), m_pEditorCamera->GetDistance() };
		const auto found = m_ViewStates.find(m_ActiveSceneKey);
		if (found != m_ViewStates.end() && found->second == live)
		{
			m_ViewIdleSeconds = 0.0f;
			return;
		}

		m_ViewIdleSeconds += delta_time;
		if (m_ViewIdleSeconds < kViewAutosaveSeconds)
			return;

		m_ViewIdleSeconds = 0.0f;
		CaptureViewState();
		StoreViewStates();
	}

	void SceneEditorLayer::SetActiveScene(const SharedPtr<Scene>& scene, const std::string& path)
	{
		PROFILE_FUNCTION();

		/* 旧场景的视角先记下来，等新场景接好再一起落盘 */
		FinishGrassPaint();
		CaptureViewState();

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

		/* 接回这个场景上次的视角（第一次打开没有记录就保持当前视角） */
		ApplyViewState();

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
		Panel::SetVisible(Panel::kScene, true);

		/* 一次写入：旧场景的视角、新场景名、接回来的视角都定下来了 */
		StoreViewStates();
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
		return Panel::IsVisible(Panel::kTimeline) && m_TimelinePanel.WantsArrowKeys();
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
		Panel::SetVisible(Panel::kTimeline, true);
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

		FinishGrassPaint();
		m_PlayMode = mode;
		OnPlayModeChanged();
	}

	/* 脚本执行顺序总表：表格里改的是草稿，点 Apply 才写进组件（一条命令，可撤销） */
	void SceneEditorLayer::DrawScriptOrderWindow()
	{
		if (!Panel::IsVisible(Panel::kScriptOrder) || m_pMainScene == nullptr)
			return;

		PanelWindow window(Panel::kScriptOrder, &Panel::VisibleFlag(Panel::kScriptOrder));
		if (!window)
			return;

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
		FinishGrassPaint();

		/* 若当前场景从未保存过，需要先确定一个保存路径 */
		if (m_ActiveScenePath.empty())
			SetActiveScenePath(FileDialog::SaveFile("scene(*.scn)\0*.scn\0"));

		/* 用户取消了路径选择：不写出空路径 */
		if (m_ActiveScenePath.empty())
			return;

		if (!SaveEditedTerrains())
			return;
		m_pMainScene->Serializer(m_ActiveScenePath);

		/* 场景内容与文件一致了：当前位置成为新的干净点 */
		MarkSceneSaved();

		/* 顺手记下视角：保存是用户认账这个场景的时刻，此时落盘不会丢 */
		CaptureViewState();
		StoreViewStates();
	}

	/* 保存场景到指定路径 */
	void SceneEditorLayer::SaveSceneAs()
	{
		PROFILE_FUNCTION();

		const std::string file_path = FileDialog::SaveFile("scene(*.scn)\0*.scn\0");

		/* 用户取消：保持当前场景路径不变 */
		if (file_path.empty())
			return;

		if (!SaveEditedTerrains())
			return;
		m_pMainScene->Serializer(file_path);

		/* 保存成功后，当前场景路径切换为新路径 */
		SetActiveScenePath(file_path);
		MarkSceneSaved();

		/* 视角换个名字记在新路径下，原路径那份留着 */
		CaptureViewState();
		StoreViewStates();
	}

	bool SceneEditorLayer::Undo()
	{
		PROFILE_FUNCTION();
		FinishGrassPaint();
		return m_CommandStack.Undo();
	}

	bool SceneEditorLayer::Redo()
	{
		PROFILE_FUNCTION();
		FinishGrassPaint();
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
		if (ImGui::Begin(Panel::kScene, &Panel::VisibleFlag(Panel::kScene)))
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
						OnDragItemToScene(ResolveAssetPath(relative_path));
				}
				ImGui::EndDragDropTarget();
			}

			/* 右上角视图指示器（点击 / 拖拽切换与旋转视角） */
			ShowViewGizmoUI();

			/* 地形笔刷落点 */
			ShowTerrainBrushUI();
			ShowGrassBrushUI();

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

	void SceneEditorLayer::UpdateGrassPaint(float delta_time)
	{
		const auto& settings = GrassPaint::GetSettings();
		const Entity selected = m_SceneHierarchy.GetSelectedEntity();
		const bool painting = m_PlayMode == PlayMode::Edit
			&& settings.Brush != GrassPaint::Mode::None && selected
			&& selected.HasComponent<GrassComponent>() && m_IsViewportHovered
			&& Input::IsMouseButtonPressed(Mouse::ButtonLeft) && !IsGizmoHovered()
			&& !ImGui::IsAnyItemHovered() && !Input::IsKeyPressed(Key::LeftAlt);
		if (!painting)
		{
			FinishGrassPaint();
			return;
		}
		if (m_GrassPaintActive && m_GrassPaintTarget != selected)
			FinishGrassPaint();
		auto& grass = selected.GetComponent<GrassComponent>();
		if (grass.m_Field == nullptr || grass.m_Field->IsEmpty())
			return;
		glm::vec3 origin(0.0f), direction(0.0f), point(0.0f);
		if (!BuildViewportRay(origin, direction)
			|| !grass.m_Field->RaycastDensity(origin, direction, settings.Radius, point))
			return;
		if (!m_GrassPaintActive)
		{
			m_GrassMaskBefore = grass.m_DensityMask;
			m_GrassPaintTarget = selected;
			m_GrassPaintActive = true;
		}
		const float amount = std::min(settings.Strength * delta_time, 1.0f)
			* (settings.Brush == GrassPaint::Mode::Add ? 1.0f : -1.0f);
		grass.m_Field->PaintDensity(grass, point, settings.Radius, amount);
	}

	void SceneEditorLayer::FinishGrassPaint()
	{
		if (!m_GrassPaintActive)
			return;
		const Entity entity = m_GrassPaintTarget;
		m_GrassPaintActive = false;
		m_GrassPaintTarget = {};
		if (!entity || !entity.HasComponent<GrassComponent>())
		{
			m_GrassMaskBefore.clear();
			return;
		}
		constexpr uint32_t n = GrassComponent::kDensityMaskSize;
		const auto& after_mask = entity.GetComponent<GrassComponent>().m_DensityMask;
		if (after_mask.size() != size_t(n) * n
			|| (!m_GrassMaskBefore.empty() && m_GrassMaskBefore.size() != size_t(n) * n))
		{
			m_GrassMaskBefore.clear();
			return;
		}
		glm::uvec2 lo(n), hi(0u);
		for (uint32_t z = 0; z < n; ++z)
			for (uint32_t x = 0; x < n; ++x)
			{
				const size_t index = size_t(z) * n + x;
				const uint8_t before = m_GrassMaskBefore.empty() ? 255u : m_GrassMaskBefore[index];
				if (before == after_mask[index]) continue;
				lo = glm::min(lo, glm::uvec2(x, z));
				hi = glm::max(hi, glm::uvec2(x, z));
			}
		if (lo.x < n)
		{
			std::vector<uint8_t> before, after;
			for (uint32_t z = lo.y; z <= hi.y; ++z)
				for (uint32_t x = lo.x; x <= hi.x; ++x)
				{
					const size_t index = size_t(z) * n + x;
					before.push_back(m_GrassMaskBefore.empty() ? 255u : m_GrassMaskBefore[index]);
					after.push_back(after_mask[index]);
				}
			m_CommandStack.Execute(CreateUniquePtr<GrassPaintCommand>(entity, lo, hi,
				std::move(before), std::move(after), m_GrassMaskBefore.empty()));
		}
		m_GrassMaskBefore.clear();
	}

	void SceneEditorLayer::ShowGrassBrushUI()
	{
		if (m_PlayMode != PlayMode::Edit || !m_IsViewportHovered
			|| GrassPaint::GetSettings().Brush == GrassPaint::Mode::None)
			return;
		const Entity selected = m_SceneHierarchy.GetSelectedEntity();
		if (!selected || !selected.HasComponent<GrassComponent>())
			return;
		const auto& grass = selected.GetComponent<GrassComponent>();
		if (grass.m_Field == nullptr) return;
		glm::vec3 origin(0.0f), direction(0.0f), point(0.0f);
		if (!BuildViewportRay(origin, direction)
			|| !grass.m_Field->RaycastDensity(origin, direction,
				GrassPaint::GetSettings().Radius, point)) return;
		const glm::mat4 vp = m_pEditorCamera->GetProjectionMatrix() * m_pEditorCamera->GetViewMatrix();
		const ImVec2 min(m_ViewportRegion.MinX, m_ViewportRegion.MinY);
		const ImVec2 size(float(m_ViewportRegion.Width), float(m_ViewportRegion.Height));
		const auto project = [&](const glm::vec3& world, ImVec2& screen)
		{
			const glm::vec4 clip = vp * glm::vec4(world, 1.0f);
			if (clip.w <= 1e-4f || clip.z < 0.0f || clip.z > clip.w)
				return false;
			screen = ImVec2(min.x + (clip.x / clip.w * 0.5f + 0.5f) * size.x,
				min.y + (0.5f - clip.y / clip.w * 0.5f) * size.y);
			return std::isfinite(screen.x) && std::isfinite(screen.y);
		};
		const float radius = GrassPaint::GetSettings().Radius;
		const glm::vec3 axis_x = grass.m_Field->GetPaintAxisX();
		const glm::vec3 axis_z = grass.m_Field->GetPaintAxisZ();
		ImDrawList* draw = ImGui::GetWindowDrawList();
		draw->PushClipRect(min, ImVec2(min.x + size.x, min.y + size.y), true);
		constexpr int segments = 64;
		std::vector<ImVec2> arc;
		for (int i = 0; i <= segments; ++i)
		{
			const float angle = float(i) * 6.2831853f / float(segments);
			glm::vec3 world = point + (axis_x * std::cos(angle) + axis_z * std::sin(angle)) * radius;
			bool valid = true;
			if (const TerrainField* ground = grass.m_Field->GetGround())
				valid = ground->SampleHeightWorld(glm::vec2(world.x, world.z), world.y);
			ImVec2 screen;
			if (valid && project(world + glm::vec3(0.0f, 0.05f, 0.0f), screen))
				arc.push_back(screen);
			else
			{
				if (arc.size() > 1)
					draw->AddPolyline(arc.data(), int(arc.size()), IM_COL32(84, 212, 150, 220),
						ImDrawFlags_None, 2.0f);
				arc.clear();
			}
		}
		if (arc.size() > 1)
			draw->AddPolyline(arc.data(), int(arc.size()), IM_COL32(84, 212, 150, 220),
				ImDrawFlags_None, 2.0f);
		ImVec2 center;
		if (project(point, center))
		{
			const ImU32 color = IM_COL32(84, 212, 150, 245);
			draw->AddLine(ImVec2(center.x - 7.0f, center.y), ImVec2(center.x + 7.0f, center.y), color, 2.0f);
			draw->AddLine(ImVec2(center.x, center.y - 7.0f), ImVec2(center.x, center.y + 7.0f), color, 2.0f);
		}
		draw->PopClipRect();
	}

	/* 地形笔刷：按住左键在选中地形上拖，抬起来算一笔。
	 * 半径按世界米给，进地形前按实体缩放换成局部米，笔刷在屏幕上的大小才稳定 */
	void SceneEditorLayer::UpdateTerrainSculpt(float delta_time)
	{
		PROFILE_FUNCTION();

		const TerrainSculpt::Settings& settings = TerrainSculpt::GetSettings();
		const bool painting = settings.Brush != TerrainSculpt::Mode::None
			&& m_IsViewportHovered
			&& Input::IsMouseButtonPressed(Mouse::ButtonLeft)
			&& !IsGizmoHovered() && !ImGui::IsAnyItemHovered()
			&& !Input::IsKeyPressed(Key::LeftAlt)
			&& TerrainSculpt::GetExportRequest().Status != TerrainSculpt::ExportStatus::Pending;

		if (!painting)
		{
			FinishSculptStroke();
			return;
		}

		Entity entity;
		glm::vec2 local(0.0f);
		glm::vec2 radius(0.0f);
		if (!RaycastSculptTarget(entity, local, radius))
			return;
		auto& terrain = entity.GetComponent<TerrainComponent>();

		if (!m_SculptActive)
		{
			terrain.m_Field->BeginBrushStroke();
			m_SculptActive = true;
			m_SculptTerrain = entity;
		}

		TerrainField::BrushMode mode = TerrainField::BrushMode::Raise;
		float amount = settings.Strength * delta_time;
		switch (settings.Brush)
		{
		case TerrainSculpt::Mode::Raise: mode = TerrainField::BrushMode::Raise; break;
		case TerrainSculpt::Mode::Lower: mode = TerrainField::BrushMode::Lower; break;
		case TerrainSculpt::Mode::Smooth:
			mode = TerrainField::BrushMode::Smooth;
			/* 抹平按比例混，一帧最多整份 */
			amount = std::min(amount, 1.0f);
			break;
		default:
			return;
		}

		glm::uvec2 min(0);
		glm::uvec2 max(0);
		terrain.m_Field->ApplyBrush(local, radius, amount, mode, min, max);
	}

	bool SceneEditorLayer::RaycastSculptTarget(Entity& out_entity, glm::vec2& out_local, glm::vec2& out_radius) const
	{
		const Entity entity = m_SceneHierarchy.GetSelectedEntity();
		if (!entity || !entity.HasComponent<TerrainComponent>())
			return false;

		const auto& terrain = entity.GetComponent<TerrainComponent>();
		if (terrain.m_Field == nullptr || terrain.m_Field->IsEmpty())
			return false;

		glm::vec3 origin(0.0f);
		glm::vec3 direction(0.0f);
		float distance = 0.0f;
		if (!BuildViewportRay(origin, direction)
			|| !terrain.m_Field->Raycast(origin, direction, out_local, distance))
			return false;

		/* 世界米换成局部米：两轴各除各的缩放 */
		const glm::vec3 world_scale = glm::max(terrain.m_Field->GetWorldScale(), glm::vec3(1e-3f));
		out_radius = glm::max(glm::vec2(TerrainSculpt::GetSettings().Radius) / glm::vec2(world_scale.x, world_scale.z),
			glm::vec2(0.05f));
		out_entity = entity;
		return true;
	}

	/* 笔刷预览：在落笔处沿地表画一圈，半径与真正改高度的范围一致 */
	void SceneEditorLayer::ShowTerrainBrushUI()
	{
		if (m_PlayMode != PlayMode::Edit || m_pEditorCamera == nullptr
			|| TerrainSculpt::GetSettings().Brush == TerrainSculpt::Mode::None || !m_IsViewportHovered
			|| m_ViewportRegion.Width <= 0 || m_ViewportRegion.Height <= 0)
			return;

		Entity entity;
		glm::vec2 center(0.0f);
		glm::vec2 radius(0.0f);
		if (!RaycastSculptTarget(entity, center, radius))
			return;

		const TerrainField& field = *entity.GetComponent<TerrainComponent>().m_Field;
		const glm::mat4 view_projection = m_pEditorCamera->GetProjectionMatrix() * m_pEditorCamera->GetViewMatrix();
		const ImVec2 viewport_min(m_ViewportRegion.MinX, m_ViewportRegion.MinY);
		const ImVec2 viewport_size(static_cast<float>(m_ViewportRegion.Width), static_cast<float>(m_ViewportRegion.Height));

		const bool paused = Input::IsKeyPressed(Key::LeftAlt) || IsGizmoHovered() || ImGui::IsAnyItemHovered()
			|| TerrainSculpt::GetExportRequest().Status == TerrainSculpt::ExportStatus::Pending;
		const auto state = paused ? TerrainBrushPreview::State::Paused : m_SculptActive
			? TerrainBrushPreview::State::Painting : TerrainBrushPreview::State::Hover;
		TerrainBrushPreview::Draw(ImGui::GetWindowDrawList(), field, center, radius,
			view_projection, viewport_min, viewport_size, state);
	}

	/* 一笔结束：把改动的矩形存成一条命令，撤销按它回填 */
	void SceneEditorLayer::FinishSculptStroke()
	{
		if (!m_SculptActive)
			return;

		const Entity entity = m_SculptTerrain;
		m_SculptActive = false;
		m_SculptTerrain = {};

		if (!entity || !entity.HasComponent<TerrainComponent>())
			return;

		auto& terrain = entity.GetComponent<TerrainComponent>();
		if (terrain.m_Field == nullptr)
			return;

		glm::uvec2 min(0);
		glm::uvec2 max(0);
		std::vector<float> before;
		std::vector<float> after;
		if (!terrain.m_Field->EndBrushStroke(min, max, before, after))
			return;

		m_CommandStack.Execute(CreateUniquePtr<TerrainSculptCommand>(m_pMainScene, entity,
			terrain.m_Field->GetResolution(), min, max, std::move(before), std::move(after)));
	}

	bool SceneEditorLayer::BuildViewportRay(glm::vec3& out_origin, glm::vec3& out_direction) const
	{
		if (m_pEditorCamera == nullptr || m_ViewportRegion.Width <= 0 || m_ViewportRegion.Height <= 0)
			return false;

		auto [mouse_x, mouse_y] = ImGui::GetMousePos();
		const float local_x = mouse_x - static_cast<float>(m_ViewportRegion.MinX);
		const float local_y = mouse_y - static_cast<float>(m_ViewportRegion.MinY);
		const float width = static_cast<float>(m_ViewportRegion.Width);
		const float height = static_cast<float>(m_ViewportRegion.Height);
		if (local_x < 0.0f || local_y < 0.0f || local_x > width || local_y > height)
			return false;

		const float ndc_x = local_x / width * 2.0f - 1.0f;
		const float ndc_y = 1.0f - local_y / height * 2.0f;

		/* 深度约定是 ZO：近平面 z = 0、远平面 z = 1 */
		const glm::mat4 inverse = glm::inverse(
			m_pEditorCamera->GetProjectionMatrix() * m_pEditorCamera->GetViewMatrix());
		const glm::vec4 near_clip = inverse * glm::vec4(ndc_x, ndc_y, 0.0f, 1.0f);
		const glm::vec4 far_clip = inverse * glm::vec4(ndc_x, ndc_y, 1.0f, 1.0f);
		if (std::fabs(near_clip.w) < 1e-6f || std::fabs(far_clip.w) < 1e-6f)
			return false;

		const glm::vec3 near_point = glm::vec3(near_clip) / near_clip.w;
		const glm::vec3 far_point = glm::vec3(far_clip) / far_clip.w;
		const glm::vec3 delta = far_point - near_point;
		if (glm::length(delta) < 1e-6f)
			return false;

		out_origin = near_point;
		out_direction = glm::normalize(delta);
		return true;
	}

	/* 导出地形：绑定模式写回原资产，非绑定模式新建不重名副本。 */
	bool SceneEditorLayer::ExportTerrain(Entity entity, bool bind_asset)
	{
		if (!entity || !entity.HasComponent<TerrainComponent>())
			return false;

		auto& terrain = entity.GetComponent<TerrainComponent>();
		if (terrain.m_Field == nullptr || terrain.m_Field->IsEmpty())
			return false;

		std::filesystem::path path;
		if (bind_asset && !terrain.m_AssetPath.empty())
			path = PathFromUtf8(terrain.m_AssetPath);
		else
		{
			std::string name = "Terrain";
			if (entity.HasComponent<NameComponent>())
			{
				const std::string& entity_name = entity.GetComponent<NameComponent>().m_Name;
				if (!entity_name.empty())
					name = entity_name;
			}
			/* 实体名当文件名用：路径分隔符与非法字符换成下划线 */
			for (char& character : name)
			{
				if (character == '/' || character == '\\' || character == ':' || character == '*'
					|| character == '?' || character == '"' || character == '<' || character == '>'
					|| character == '|')
					character = '_';
			}

			const std::filesystem::path directory = g_ProjectPath / "Terrains";
			std::error_code error;
			std::filesystem::create_directories(directory, error);

			path = directory / (name + ".terrain");
			for (int suffix = 1; std::filesystem::exists(path, error); ++suffix)
				path = directory / (name + "_" + std::to_string(suffix) + ".terrain");
		}

		const std::string material_path = terrain.m_MaterialSlot.SourceAssetPath.empty()
			? std::string() : RELATIVE_PATH(terrain.m_MaterialSlot.SourceAssetPath);
		if (!terrain.m_Field->ExportAsset(PathToUtf8(path), material_path))
		{
			EDITOR_LOG_WARN("Terrain export failed: {}", PathToUtf8(path));
			return false;
		}

		/* 新绑了资产路径：场景里的引用变了，要提示保存 */
		if (bind_asset && terrain.m_AssetPath != PathToUtf8(path))
		{
			terrain.m_AssetPath = PathToUtf8(path);
			m_HasUnrecordedChange = true;
		}
		EDITOR_LOG_INFO("Terrain exported: {}", PathToUtf8(path));
		return true;
	}

	bool SceneEditorLayer::SaveEditedTerrains()
	{
		for (auto [handle, terrain] : m_pMainScene->GetRegistry().view<TerrainComponent>().each())
		{
			if (terrain.m_Field != nullptr && terrain.m_Field->HasUnsavedEdits()
				&& !ExportTerrain(Entity{ handle, m_pMainScene }))
				return false;
		}
		return true;
	}

	/* F 键：把轨道中心聚焦到选中实体 */
	void SceneEditorLayer::FocusSelectedEntity()
	{
		FocusEntity(m_SceneHierarchy.GetSelectedEntity());
	}

	/* 把轨道中心定位到实体，并按包围球取景（F 键与层级树双击共用） */
	void SceneEditorLayer::FocusEntity(const Entity& entity)
	{
		PROFILE_FUNCTION();

		if (m_PlayMode != PlayMode::Edit || !entity || m_pMainScene == nullptr
			|| !entity.HasComponent<TransformComponent>())
			return;

		const glm::mat4 world_transform = m_pMainScene->GetWorldTransform(entity);
		glm::vec3 focus_center = glm::vec3(world_transform[3]);
		float focus_radius = 1.0f;

		if (entity.HasComponent<ModelComponent>())
		{
			const auto& model = entity.GetComponent<ModelComponent>().m_Model;
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

		UpdateViewport();
		UpdateSceneCameraViewports();
		UpdateSceneCameraRendering();

		/* 相机列表与面板 UI 同源：即使本帧不渲染（视口为 0）也刷新，面板不读过期条目 */
		CollectCameraEntries();

		if (const Entity target = TerrainSculpt::ConsumeExportRequest())
		{
			const uint64_t serial = TerrainSculpt::GetExportRequest().Serial;
			FinishSculptStroke();
			TerrainSculpt::CompleteExport(serial, ExportTerrain(target, TerrainSculpt::GetExportRequest().BindAsset));
		}

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

				/* 笔刷在场景更新之前改高度，这一帧的草地贴地就能跟上 */
				UpdateTerrainSculpt(delta_time);
				UpdateGrassPaint(delta_time);

				/* 更新场景中的实体 */
				m_pMainScene->OnUpdate(delta_time);
				break;
			case PlayMode::Runtime:
				/* 运行模式下编辑器相机不响应输入，但编辑器视口仍要由它呈现 */
				m_pMainScene->OnUpdate(delta_time);
				break;
			}

			/* 相机这一帧的姿态定下来了，顺手看看要不要落盘 */
			UpdateViewStateAutosave(delta_time);

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

			/* 选中项 → 视口描边：id 取自实体句柄，与拾取缓冲同源。
			 * 句柄已失效（实体被删 / 换了场景）就不画。 */
			const Entity selected_entity = m_SceneHierarchy.GetSelectedEntity();
			m_pEditorCamera->SetSelectionId(
				(selected_entity && m_pMainScene->IsEntityValid(selected_entity))
					? static_cast<int32_t>(static_cast<uint32_t>(selected_entity)) : -1);

			/* 执行本帧收集到的渲染视图：场景视口的内容在这里产生 */
			m_pMainScene->Render();
		}
	}
}
