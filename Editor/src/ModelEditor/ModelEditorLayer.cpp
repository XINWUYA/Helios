#include "Pch.h"
#include "ModelEditorLayer.h"
#include "MaterialSlotEditing.h"
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cfloat>
#include <limits>
#include "Helios/Application/Application.h"
#include "Helios/ImGui/EditorTheme.h"
#include "Helios/Scene/ModelImporter.h"
#include "Helios/Scene/ModelManifest.h"
#include "DebugViewMenu.h"
#include "EditorCommon.h"
#include "EditorIcons.h"
#include "MaterialParamUI.h"
#include "PanelChrome.h"
#include "PanelRegistry.h"

namespace Helios
{
	namespace
	{
		/* === 材质参数面板（右栏） === */
		constexpr float kParamsPaneWidth = 400.0f;        /* 常态定宽（设计像素，窄窗按比例收） */
		constexpr float kParamsToggleInsetTop = 10.0f;    /* 角落开关距窗口上缘 */
		constexpr float kParamsToggleInsetRight = 14.0f;  /* 距右缘，多让一档避开面板滚动条 */
		/* === 预览工具按钮（左上角） === */
		constexpr float kPreviewToolsInsetTop = 10.0f;
		constexpr float kPreviewToolsInsetLeft = 10.0f;

		/* 参数面板开关的屏幕位置：窗口内容区右上角（打开 / 收起两态画在同一处——
		 * 打开态开关在面板子窗口里画，坐标也得按父窗口的角算） */
		ImVec2 ParamsTogglePos(const ImGuiWindow* window)
		{
			const float button_size = ImGui::GetFrameHeight();
			return ImVec2(
				window->ContentRegionRect.Max.x - Scaled(kParamsToggleInsetRight) - button_size,
				window->ContentRegionRect.Min.y + Scaled(kParamsToggleInsetTop));
		}

		/* 向量显示成一行紧凑文本：属性行里数字比括号更重要 */
		std::string FormatVec3(const glm::vec3& value)
		{
			char buffer[64] = {};
			snprintf(buffer, sizeof(buffer), "%.3f, %.3f, %.3f", value.x, value.y, value.z);
			return buffer;
		}

		/* 片段显示名：去掉导入时加的 <模型名>_ 前缀与扩展名，只留 take 名 */
		std::string ClipDisplayName(const std::string& mesh_path, const std::string& clip_path)
		{
			const std::string clip_stem = PathToUtf8(PathFromUtf8(clip_path).stem());
			const std::string prefix = PathToUtf8(PathFromUtf8(mesh_path).stem()) + "_";
			if (clip_stem.rfind(prefix, 0) == 0 && clip_stem.size() > prefix.size())
				return clip_stem.substr(prefix.size());
			return clip_stem;
		}

		float ClipDuration(const AnimatorComponent& animator)
		{
			return (animator.m_Clip != nullptr) ? animator.m_Clip->GetDuration() : 0.0f;
		}

		/* 与 .mesh 同目录的 .anim，按文件名排序。prefix 非空时只收该前缀
		 * （导入命名是 <模型名>_<片段名>.anim，别的模型的片段不掺进来） */
		std::vector<std::filesystem::path> FindSiblingClipFiles(const std::filesystem::path& mesh_path,
			const std::string& prefix)
		{
			std::vector<std::filesystem::path> clip_paths;

			std::error_code error;
			if (!std::filesystem::is_directory(mesh_path.parent_path(), error))
				return clip_paths;

			for (const auto& entry : std::filesystem::directory_iterator(mesh_path.parent_path(), error))
			{
				if (!entry.is_regular_file(error))
					continue;
				if (ToLowercase(PathToUtf8(entry.path().extension()).c_str()) != ".anim")
					continue;
				if (!prefix.empty() && PathToUtf8(entry.path().stem()).rfind(prefix, 0) != 0)
					continue;

				clip_paths.push_back(entry.path());
			}
			std::sort(clip_paths.begin(), clip_paths.end());
			return clip_paths;
		}

		/* 导出路径归一：后缀不对（或没给）时按目标类型补上 */
		std::filesystem::path ResolveExportPath(const std::string& file_path, const char* extension)
		{
			std::filesystem::path target = PathFromUtf8(file_path);
			if (ToLowercase(PathToUtf8(target.extension()).c_str()) != extension)
				target.replace_extension(extension);
			return target;
		}

		/* 导入工作区（Assets/Cache）里的引用只对这次编辑有效，导出时不能跟着写进清单 */
		bool IsScratchAssetPath(const std::string& asset_path)
		{
			if (asset_path.empty())
				return false;

			std::error_code error;
			const std::filesystem::path relative =
				std::filesystem::relative(PathFromUtf8(asset_path), g_AssetsPath / "Cache", error);
			return !error && !relative.empty()
				&& relative.native().rfind(std::filesystem::path("..").native(), 0) != 0;
		}

		/* 模型预览网格的格距：视线里约 6 格为目标，取 1/2/5 序列里最近的一档
		 * （格距与补丁半径同乘缩放，淡出范围跟着走 —— 见 EditorCamera::SetPreviewGrid） */
		float ChoosePreviewGridScale(float model_radius)
		{
			static constexpr float kCandidates[] = {
				0.05f, 0.1f, 0.25f, 0.5f, 1.0f, 2.5f, 5.0f, 10.0f, 25.0f, 50.0f, 100.0f, 250.0f
			};

			const float target = std::max(model_radius, 1.0e-3f) / 6.0f;
			float best = 1.0f;
			float best_delta = std::numeric_limits<float>::max();
			for (const float candidate : kCandidates)
			{
				const float delta = std::fabs(std::log(candidate / target));
				if (delta < best_delta)
				{
					best_delta = delta;
					best = candidate;
				}
			}
			return best;
		}
	}
	ModelEditorLayer::ModelEditorLayer()
		: ILayer("ModelEditorLayer")
	{
		PROFILE_FUNCTION();
	}

	void ModelEditorLayer::OnAttached()
	{
		PROFILE_FUNCTION();

		m_pEditorCamera = CreateUniquePtr<EditorCamera>();
		/* 模型预览用聚焦模式：左键直接绕模型旋转 + 滚轮缩放，平移 / 飞行不参与 */
		m_pEditorCamera->SetFocusMode(true);
		m_pDefaultScene = CreateSharedPtr<Scene>();
		/* 编辑器相机登记为外部相机：每帧随场景相机一起收集渲染（见 Scene::AddExternalCamera） */
		m_pDefaultScene->AddExternalCamera(m_pEditorCamera.get());
		m_pModelInfo = CreateUniquePtr<ModelInfo>();

		/* 默认在场景中增加一盏方向光，光源颜色为白色 */
		Entity entity = m_pDefaultScene->CreateEntity("DirectionalLight");
		auto& light_component = entity.AddComponent<LightComponent>(LightType::Directional);
		light_component.m_Light->SetColor(glm::vec4(1, 1, 1, 1));
		/* 强度跟场景里的方向光同档（5）：1 在引擎的光照口径下只有 ~50/255 的亮度 */
		light_component.m_Light->SetIntensity(5);
		auto& light_transform = entity.GetComponent<TransformComponent>();
		/* 光向 = 旋转作用下的 (0,-1,0)：用四元数把规范朝向对到"前上方偏一侧"的 key light，
		 * 再写回欧拉旋转 —— 默认视角正对相机的面被照亮（旧值从侧面照，画面几乎全黑） */
		const glm::vec3 light_direction = glm::normalize(glm::vec3(0.25f, -0.7f, -0.66f));
		light_transform.m_Rotation = glm::eulerAngles(
			glm::rotation(glm::vec3(0.0f, -1.0f, 0.0f), light_direction));

		/* 默认铺一片天空盒当预览背景，材质跟场景里那份共用（Materials/Sky.mtl）。
		 * 顶点着色器去掉了视图平移，相机就在球心，天空自然铺满整个视口。 */
		SharedPtr<Material> sky_material = CreateSharedPtr<Material>();
		if (MaterialIO::ReadFile("Materials/Sky.mtl", *sky_material))
		{
			/* 从球内侧看：剔掉朝外的正面，只留内表面 */
			RenderRasterState sky_raster_state = sky_material->GetRasterState();
			sky_raster_state.CullMode = CullMode::Cull_Front;
			sky_material->SetRasterState(sky_raster_state);

			Entity sky = m_pDefaultScene->CreateEntity("Sky");
			auto& sky_model = sky.AddComponent<ModelComponent>();
			sky_model.m_Model = Model::Create(BuiltinModelType::Sphere, sky_material);
			/* 天空盒不投也不收阴影 */
			sky_model.m_CastShadow = false;
			sky_model.m_ReceiveShadow = false;
		}
	}

	void ModelEditorLayer::OnDetached()
	{
		PROFILE_FUNCTION();

		/* 相机先于场景销毁：按登记契约摘除 */
		if (m_pDefaultScene && m_pEditorCamera)
			m_pDefaultScene->RemoveExternalCamera(m_pEditorCamera.get());

		ILayer::OnDetached();
	}

	void ModelEditorLayer::OnUpdate(float delta_time)
	{
		PROFILE_FUNCTION();

		if (!m_IsActivated)
			return;
		
		Renderer::SetClearColor(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
		Renderer::Clear();

		/* 相机导航：指针在视口上才授权起手 */
		m_pEditorCamera->SetNavigationAllowed(m_IsViewportHovered);
		m_pEditorCamera->OnUpdate(delta_time);

		/* 槽覆盖改过才同步到预览实体：编辑会自动换实例，换的是 map 里的指针 */
		if (m_SlotOverridesDirty && m_pDefaultScene != nullptr
			&& m_pDefaultScene->IsEntityValid(m_PreviewEntity))
		{
			if (auto* component = m_pDefaultScene->GetRegistry().try_get<ModelComponent>(m_PreviewEntity))
			{
				component->m_SlotOverrides = m_SlotOverrides;
				m_SlotOverridesDirty = false;
			}
		}

		m_pDefaultScene->OnUpdate(delta_time);

		/* 打开模型后的首帧已经把关节树建好（SkeletonModel 在动画 Tick 之后建），
		 * 现在再开播：下一次 Tick 才有关节可绑 */
		if (m_AnimationStartPending)
		{
			m_AnimationStartPending = false;
			if (AnimatorComponent* animator = GetPreviewAnimator())
			{
				animator->m_AutoPlay = true;
				animator->m_Playing = true;
				animator->m_Scrubbing = false;
			}
		}

		/* 执行本帧收集到的渲染视图：模型视口的内容在这里产生 */
		m_pDefaultScene->Render();

	}

	void ModelEditorLayer::OnImGuiRender()
	{
		PROFILE_FUNCTION();

		if (!m_IsActivated)
			return;

		/* 预览与材质参数在同一个窗口（参数是可折叠浮层） */
		ShowModelWindowUI();
	}

	void ModelEditorLayer::OnEvent(IEvent* event)
	{
		PROFILE_FUNCTION();

		if (!m_IsActivated)
			return;

		if (!event)
			return;

		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<MouseScrolledEvent>(BIND_EVENT_FUNC(ModelEditorLayer::OnMouseScrolled));
	}

	/* 滚轮：指针在视口上时拉近拉远（聚焦模式的缩放通道） */
	bool ModelEditorLayer::OnMouseScrolled(MouseScrolledEvent* event)
	{
		PROFILE_FUNCTION();

		if (!m_IsViewportHovered)
			return false;

		const float wheel = event->GetYOffset();
		if (wheel == 0.0f)
			return false;

		m_pEditorCamera->OnMouseWheelZoom(wheel);
		return true;
	}

	/* 模型编辑窗口：左右分栏 —— 左栏预览（预览工具按钮常驻左上角），右栏材质参数面板（可收起）。
	 * 窗口隐藏时不提交 Begin：本仓 ImGui 的 p_open 只做写回不做隐藏（关了不提交才真的关），
	 * 恢复入口在 View 菜单，与 Frame Graph / Log 同一约定。 */
	void ModelEditorLayer::ShowModelWindowUI()
	{
		PROFILE_FUNCTION();

		/* 窗口已关闭：不提交也不画；悬停授权与高亮一并清掉，不留指向已关窗口的残影 */
		if (!m_IsViewportVisible)
		{
			m_IsViewportHovered = false;
			m_pEditorCamera->SetHighlightSegment(-1);
			return;
		}

		const bool has_model = (m_pModelInfo != nullptr) && !m_pModelInfo->m_SubModelInfos.empty();

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		const bool is_open = ImGui::Begin(Panel::kModel, &m_IsViewportVisible);
		/* 窗口内边距在 Begin 时已取快照；提前弹掉，参数面板的子窗口按默认内边距排卡片 */
		ImGui::PopStyleVar();

		if (is_open)
		{
			/* 获取窗口范围 */
			const auto viewport_region_min = ImGui::GetWindowContentRegionMin();
			const auto viewport_region_max = ImGui::GetWindowContentRegionMax();
			const auto viewport_offset = ImGui::GetWindowPos();
			const float content_width = viewport_region_max.x - viewport_region_min.x;
			const float content_height = viewport_region_max.y - viewport_region_min.y;

			/* 右栏宽度：常态定宽一档；窗口太窄时按半宽收，给预览留出至少一半 */
			const float params_width = (has_model && m_IsParamsPanelOpen)
				? ImMin(Scaled(kParamsPaneWidth), content_width * 0.5f) : 0.0f;
			const float preview_width = ImMax(content_width - params_width, 0.0f);

			/* 预览占左栏 */
			m_ViewportRegion.MinX = viewport_region_min.x + viewport_offset.x;
			m_ViewportRegion.MinY = viewport_region_min.y + viewport_offset.y;
			/* 窗口极小时 max 可能小于 min，差值为负；赋给 uint32_t 会回绕成极大值 */
			m_ViewportRegion.Width = static_cast<uint32_t>(std::max(0.0f, preview_width));
			m_ViewportRegion.Height = static_cast<uint32_t>(std::max(0.0f, content_height));

			/* RenderTarget 按物理像素分配（macOS：ImGui 单位是点、×2 到像素；Windows：已是物理像素、×1，
			 * 见 ImGuiToPhysicalScale）；ClampRenderSize 负责负值/超大值防护。 */
			const auto rt_size = ClampRenderSize(preview_width, content_height, ImGuiToPhysicalScale());
			m_pEditorCamera->SetViewportRegion({ 0, 0, rt_size.x, rt_size.y });

			/* 打开模型后的取景：等这里视口尺寸 / 宽高比就绪（FocusOn 按紧的一侧取景） */
			if (m_AutoFocusPending && m_pModel != nullptr)
			{
				const glm::vec3 aabb_min = m_pModel->GetAABBMin();
				const glm::vec3 aabb_max = m_pModel->GetAABBMax();
				m_pEditorCamera->FocusOn((aabb_min + aabb_max) * 0.5f,
					glm::max(glm::length(aabb_max - aabb_min) * 0.5f, 0.1f));
				m_AutoFocusPending = false;
			}

			/* 绘制场景；图像上再叠一个无形输入区（与图像同尺寸，有无渲染目标都提交）——
			 * 视口因此有了真实的 item：悬停 / 按下都占住命中，ImGui 的"点窗口空白拖窗口"
			 * 逻辑不再把转模型当成拖标题栏；相机悬停授权也认这一个 item */
			const ImVec2 image_pos = ImGui::GetCursorScreenPos();
			const auto viewport_panel_size = ImVec2(preview_width, content_height);
			bool viewport_hovered = false;
			if (auto output_rt = m_pEditorCamera->GetRenderView()->GetRenderTarget())
			{
				/* ImTextureID 统一存放 DeviceTexture 指针：GetTextureID() 返回的硬件
				 * 句柄在 Metal 上是 64 位指针被截断后的 32 位，不能当指针使用。 */
				ImGui::Image((ImTextureID)output_rt.get(), viewport_panel_size, ImVec2{ 0, 1 }, ImVec2{ 1, 0 });
			}

			/* 空状态提示（没有模型时）：画在输入区之前 —— 输入区占满整块视口，
			 * 画在它之后光标已被挤到区外，文字就落进看不见的地方了 */
			if (!has_model)
				ShowEmptyModelHint(image_pos, viewport_panel_size);

			ImGui::SetCursorScreenPos(image_pos);
			ImGui::InvisibleButton("##ViewportInput", viewport_panel_size);
			viewport_hovered = ImGui::IsItemHovered();
			/* 左上角工具按钮与右上角开关压在视口上：允许后画的控件在自己的角上抢走命中 */
			ImGui::SetItemAllowOverlap();

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

			/* 预览工具按钮 + 材质参数面板（没打开模型时只有空状态提示，提示已先画） */
			m_IsParamsToggleHovered = false;
			m_IsPreviewToolsHovered = false;
			m_HighlightedSubModel = -1;
			if (has_model)
			{
				ShowPreviewTools();
				ShowMaterialParamsPane();
			}

			/* 指针悬停授权：图像本体 hovered（面板是独立子窗口，悬停它时图像不算 hovered），
			 * 且指针不压在角落悬浮控件上（面板开关 / 预览工具按钮盖在视口角上，会抢走命中） */
			m_IsViewportHovered = viewport_hovered && !m_IsParamsToggleHovered && !m_IsPreviewToolsHovered;

			/* 子网格高亮推给相机：下个渲染帧生效（见 EditorCamera::SetHighlightSegment） */
			m_pEditorCamera->SetHighlightSegment(m_HighlightedSubModel);
		}
		else
		{
			/* 视口窗口不可见：清掉悬停，避免残留的起手授权 */
			m_IsViewportHovered = false;
			m_pEditorCamera->SetHighlightSegment(-1);
		}
		ImGui::End();
	}

	/* 材质参数面板（右栏）：最上方标题栏显示当前模型名，下面基础信息卡 + 每个子模型一张卡。
	 * 内容只上下滚（常驻竖向滚动条，滚动条宽度恒定预留）；卡片按可用宽度排，横向恒不可滑。
	 * 收起时只留右上角开关，开关画在视口上（没有子窗口遮挡，父窗口的控件可点）。 */
	void ModelEditorLayer::ShowMaterialParamsPane()
	{
		PROFILE_FUNCTION();

		if (!m_IsParamsPanelOpen)
		{
			if (DrawParamsPaneToggle())
				m_IsParamsPanelOpen = true;
			return;
		}

		const ImVec2 region_min = ImGui::GetWindowContentRegionMin();
		const ImVec2 region_max = ImGui::GetWindowContentRegionMax();
		const float content_width = region_max.x - region_min.x;
		/* 常态定宽一档；窗口太窄时按半宽收，给视口留出至少一半 */
		const float pane_width = ImMin(Scaled(kParamsPaneWidth), content_width * 0.5f);

		ImGui::SetCursorPos(ImVec2(region_max.x - pane_width, region_min.y));
		ImGui::PushStyleColor(ImGuiCol_ChildBg,
			EditorTheme::WithAlpha(EditorTheme::Token::Neutral3, 0.97f));
		/* 面板本身不滚（滚的是下面的内容区）；标题栏再长也不给面板留出横向滑动区 */
		ImGui::BeginChild("##ModelParamsPane", ImVec2(pane_width, region_max.y - region_min.y), true,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		{
			/* 开关画进面板里（打开态面板盖住父窗口的控件，点不到）：标题行仍从内容区顶格开始 */
			const ImVec2 content_start = ImGui::GetCursorScreenPos();
			if (DrawParamsPaneToggle())
				m_IsParamsPanelOpen = false;
			ImGui::SetCursorScreenPos(content_start);

			/* 标题栏：当前模型名（右端给开关让位） */
			{
				ImGuiWindow* window = ImGui::GetCurrentWindow();
				if (window->ParentWindow != nullptr)
					window = window->ParentWindow;

				const PanelChrome::HeaderRow header = PanelChrome::BeginHeaderRow(Icons::Id::Model);
				const std::string model_name =
					PathToUtf8(PathFromUtf8(m_pModelInfo->m_Path).filename());
				const float title_width = ImMax(
					ParamsTogglePos(window).x - header.TitleX - ImGui::GetStyle().ItemInnerSpacing.x,
					1.0f);
				PanelChrome::DrawHeaderTitle(header, model_name, title_width);
				PanelChrome::EndHeaderRow(header);
			}

			/* 内容：只上下滚（常驻竖向滚动条 + 卡片按可用宽度排，横向不含可滑区） */
			ImGui::BeginChild("##ModelParamsContent", ImVec2(0.0f, 0.0f), false,
				ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_AlwaysVerticalScrollbar);
			ShowModelBaseInfoCard();
			ShowExportCard();
			ShowAnimationCard();
			ShowSubModelCards();
			ImGui::EndChild();
		}
		ImGui::EndChild();
		ImGui::PopStyleColor();
	}

	/* 面板的折叠开关：窗口右上角一枚图标按钮，打开 / 收起两态位置一致。
	 * 屏幕位置按父窗口内容区算 —— 打开态在面板里画，坐标也得用父窗口的角 */
	bool ModelEditorLayer::DrawParamsPaneToggle()
	{
		PROFILE_FUNCTION();

		ImGuiWindow* window = ImGui::GetCurrentWindow();
		if (window->ParentWindow != nullptr)
			window = window->ParentWindow;

		const float button_size = ImGui::GetFrameHeight();
		ImGui::SetCursorScreenPos(ParamsTogglePos(window));

		const bool clicked = Icons::IconButton(Icons::Id::Sliders, ImVec2(button_size, button_size),
			m_IsParamsPanelOpen, "Material Parameters");
		/* 收起态开关盖在视口上（会抢走视口的悬停命中）：供帧末结算视口悬停时排除 */
		m_IsParamsToggleHovered = ImGui::IsItemHovered();
		return clicked;
	}

	/* 预览工具（左上角并排的三个图标按钮）：保存模型 / Debug View / 重置视图。
	 * 位置与参数面板开关同款（按父窗口内容区算屏幕位置）—— 左上角不压任何子窗口、
	 * 也不随面板收起，始终画在父窗口上；按钮悬停会挡掉视口悬停（见 ShowModelWindowUI 帧末结算） */
	void ModelEditorLayer::ShowPreviewTools()
	{
		PROFILE_FUNCTION();

		ImGuiWindow* window = ImGui::GetCurrentWindow();
		if (window->ParentWindow != nullptr)
			window = window->ParentWindow;

		const ImGuiStyle& style = ImGui::GetStyle();
		const float button_size = ImGui::GetFrameHeight();
		ImGui::SetCursorScreenPos(ImVec2(
			window->ContentRegionRect.Min.x + Scaled(kPreviewToolsInsetLeft),
			window->ContentRegionRect.Min.y + Scaled(kPreviewToolsInsetTop)));

		bool hovered = false;

		if (Icons::IconButton(Icons::Id::Save, ImVec2(button_size, button_size),
			false, "Save Model (.model + .mesh + .mtl + .anim)"))
			ExportMeshAndMtl();
		hovered |= ImGui::IsItemHovered();

		/* Debug View：与场景视口同一份档位表（见 DebugViewMenu.h），按钮点亮 = 不是 Shaded 档 */
		ImGui::SameLine(0.0f, style.ItemSpacing.x);
		const DebugViewMode debug_view = m_pEditorCamera->GetDebugViewMode();
		const bool debug_menu_open = ImGui::IsPopupOpen("##ModelDebugViewMenu");
		if (debug_menu_open)
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
		if (Icons::IconButton(Icons::Id::DebugView, ImVec2(button_size, button_size),
			debug_view != DebugViewMode::None, "Debug View"))
			ImGui::OpenPopup("##ModelDebugViewMenu");
		if (debug_menu_open)
			ImGui::PopStyleColor();
		hovered |= ImGui::IsItemHovered();

		/* 弹层贴在按钮左下；档位行点选不收起，连点切档 */
		ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + Scaled(4.0f)),
			ImGuiCond_Appearing);
		ImGui::SetNextWindowSizeConstraints(ImVec2(DebugViewMenu::MinimumMenuWidth(), 0.0f),
			ImVec2(FLT_MAX, FLT_MAX));
		if (ImGui::BeginPopup("##ModelDebugViewMenu"))
		{
			/* 编辑器的预览相机缺省走延迟管线；前向时光照分量置灰（与场景视口同规则） */
			DebugViewMenu::DrawItems(debug_view,
				m_pEditorCamera->GetRenderPipeline() == RenderPipeline::Deferred,
				[this](DebugViewMode mode) { m_pEditorCamera->SetDebugViewMode(mode); });
			ImGui::EndPopup();
		}

		ImGui::SameLine(0.0f, style.ItemSpacing.x);
		if (Icons::IconButton(Icons::Id::FitView, ImVec2(button_size, button_size),
			false, "Reset View"))
			m_AutoFocusPending = true;
		hovered |= ImGui::IsItemHovered();

		m_IsPreviewToolsHovered = hovered;
	}

	/* 基本信息：整份模型的路径、包围盒与规模 */
	void ModelEditorLayer::ShowModelBaseInfoCard()
	{
		PROFILE_FUNCTION();

		const PanelChrome::Card card = PanelChrome::BeginCard("Base Info", Icons::Id::Model);

		if (card.Open)
		{
			ImGuiExt::DrawCommonTextUI("Path", m_pModelInfo->m_Path);
			ImGuiExt::DrawCommonTextUI("AABB Min", FormatVec3(m_pModelInfo->m_AABB.first));
			ImGuiExt::DrawCommonTextUI("AABB Max", FormatVec3(m_pModelInfo->m_AABB.second));

			size_t vertex_count = 0;
			for (const auto& sub_model_info : m_pModelInfo->m_SubModelInfos)
				vertex_count += sub_model_info->VertexArray->GetVertexCount();
			ImGuiExt::DrawCommonTextUI("Vertices", std::to_string(vertex_count));
			ImGuiExt::DrawCommonTextUI("Mesh Segments",
				std::to_string(m_pModelInfo->m_SubModelInfos.size()));
		}

		PanelChrome::EndCard(card);
	}

	/* 导出卡（右栏）：模型 / 材质 / 动画各走各的对话框，只写选中的那一种文件。
	 * 左上角的一键保存是整包（.mesh + .mtl + 贴图 + 片段），这里用于单独出一份资产 */
	void ModelEditorLayer::ShowExportCard()
	{
		PROFILE_FUNCTION();

		if (m_pModel == nullptr || m_pModelInfo == nullptr)
			return;

		const PanelChrome::Card card = PanelChrome::BeginCard("Export", Icons::Id::Export);

		if (card.Open)
		{
			const auto draw_export_row = [this](const char* label, const char* button_text,
				const std::string& tooltip, bool enabled, void (ModelEditorLayer::*action)())
			{
				const float value_width = ImGuiExt::BeginPropertyRow(label);

				ImGui::BeginDisabled(!enabled);
				if (ImGui::Button(button_text, ImVec2(value_width, 0.0f)))
					(this->*action)();
				ImGui::EndDisabled();

				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					ImGui::SetTooltip("%s", tooltip.c_str());

				ImGuiExt::EndPropertyRow();
			};

			draw_export_row("Model", "Export .mesh", "Copy the model geometry (.mesh) to a chosen file",
				true, &ModelEditorLayer::ExportModelOnly);

			/* 多槽模型先选槽：默认第一条，导出的是选中的那一份 */
			const auto& slots = m_pModel->GetMaterialSlots();
			if (slots.size() > 1)
			{
				std::vector<std::string> names;
				names.reserve(slots.size());
				for (const MaterialSlot& slot : slots)
					names.push_back(slot.Name);

				m_ExportSlot = std::clamp(m_ExportSlot, 0, static_cast<int>(names.size()) - 1);
				ImGuiExt::DrawComboUI("Slot", names, m_ExportSlot,
					[this](int picked) { m_ExportSlot = picked; });
			}

			std::string material_tooltip = "Write the edited material (.mtl) to a chosen file";
			if (!slots.empty())
			{
				const int index = std::clamp(m_ExportSlot, 0, static_cast<int>(slots.size()) - 1);
				material_tooltip = "Export '" + slots[static_cast<size_t>(index)].Name + "' (.mtl) to a chosen file";
			}
			draw_export_row("Material", "Export .mtl", material_tooltip,
				!slots.empty(), &ModelEditorLayer::ExportMaterialOnly);

			/* 动画导的是当前选中的片段：把它的名字带进提示里 */
			std::string animation_tooltip = "No animation clip";
			const SharedPtr<AnimationClip>* selected =
				(m_SelectedClip >= 0 && m_SelectedClip < static_cast<int>(m_AnimationClips.size()))
				? &m_AnimationClips[static_cast<size_t>(m_SelectedClip)] : nullptr;
			if (selected != nullptr && *selected != nullptr)
			{
				animation_tooltip = "Export '" + ClipDisplayName(m_pModelInfo->m_Path, (*selected)->GetPath())
					+ "' (.anim) to a chosen file";
			}
			draw_export_row("Animation", "Export .anim", animation_tooltip,
				selected != nullptr && *selected != nullptr, &ModelEditorLayer::ExportSelectedAnimation);
		}

		PanelChrome::EndCard(card);
	}

	/* 单独导出模型：只拷 .mesh（骨骼段原样带走），不写材质与片段 */
	void ModelEditorLayer::ExportModelOnly()
	{
		if (!m_pModelInfo || !m_pModel)
			return;

		const std::string default_name = PathToUtf8(PathFromUtf8(m_pModelInfo->m_Path).filename());
		const auto file_path = FileDialog::SaveFile("Mesh(*.mesh)\0*.mesh\0", default_name.c_str());
		if (file_path.empty())
			return;

		ExportModelTo(file_path);
	}

	/* 单独导出材质：写选中槽的那一条 .mtl */
	void ModelEditorLayer::ExportMaterialOnly()
	{
		if (!m_pModelInfo || !m_pModel)
			return;

		std::filesystem::path default_path = PathFromUtf8(m_pModelInfo->m_Path);
		default_path.replace_extension(".mtl");
		const auto file_path = FileDialog::SaveFile("Material(*.mtl)\0*.mtl\0",
			PathToUtf8(default_path.filename()).c_str());
		if (file_path.empty())
			return;

		ExportMaterialTo(file_path, m_ExportSlot);
	}

	/* 单独导出动画：只写当前选中片段的 .anim，默认文件名用片段名（不带模型前缀） */
	void ModelEditorLayer::ExportSelectedAnimation()
	{
		if (m_SelectedClip < 0 || m_SelectedClip >= static_cast<int>(m_AnimationClips.size()))
			return;

		const SharedPtr<AnimationClip>& clip = m_AnimationClips[static_cast<size_t>(m_SelectedClip)];
		if (clip == nullptr)
			return;

		const std::string default_name = ClipDisplayName(m_pModelInfo->m_Path, clip->GetPath()) + ".anim";
		const auto file_path = FileDialog::SaveFile("Animation(*.anim)\0*.anim\0", default_name.c_str());
		if (file_path.empty())
			return;

		ExportAnimationTo(file_path);
	}

	/* 动画卡（右栏）：打开带骨骼动画的模型时自动挂上的动画组件，在这里就是一张卡 ——
	 * 片段、播放、循环、速度、时间轴都在这里改，预览下一帧跟着动。
	 * 模型没带片段（组件没挂）时整张卡不出现 */
	void ModelEditorLayer::ShowAnimationCard()
	{
		PROFILE_FUNCTION();

		AnimatorComponent* animator = GetPreviewAnimator();
		if (animator == nullptr || m_AnimationClips.empty())
			return;

		const PanelChrome::Card card = PanelChrome::BeginCard("Animation", Icons::Id::Animation);
		if (card.Open)
		{
			/* 片段：单个直接显示，多个 take 给下拉切换 */
			if (m_AnimationClips.size() > 1)
			{
				std::vector<std::string> names;
				names.reserve(m_AnimationClips.size());
				for (const auto& clip : m_AnimationClips)
				{
					names.push_back(clip != nullptr
						? ClipDisplayName(m_pModelInfo->m_Path, clip->GetPath()) : std::string("(missing)"));
				}

				int selected = (m_SelectedClip >= 0 && m_SelectedClip < static_cast<int>(m_AnimationClips.size()))
					? m_SelectedClip : 0;
				ImGuiExt::DrawComboUI("Clip", names, selected,
					[this, animator](int index)
					{
						m_SelectedClip = index;
						animator->m_Clip = m_AnimationClips[static_cast<size_t>(index)];
						animator->m_Loop = animator->m_Clip->IsLoop();
						animator->m_Time = 0.0f;
						animator->m_Playing = true;
						animator->m_Scrubbing = false;
					});
			}
			else
			{
				const SharedPtr<AnimationClip>& clip = m_AnimationClips.front();
				ImGuiExt::DrawCommonTextUI("Clip", clip != nullptr
					? ClipDisplayName(m_pModelInfo->m_Path, clip->GetPath()) : std::string("(missing)"));
			}

			/* 播放 / 暂停：暂停停在当前姿势，跟时间轴面板同一套语义 */
			{
				ImGuiExt::BeginPropertyRow("Playback");

				const float button_size = ImGui::GetFrameHeight();
				const bool playing = animator->m_Playing;
				if (Icons::IconButton(playing ? Icons::Id::Pause : Icons::Id::Play,
					ImVec2(button_size, button_size), playing, playing ? "Pause" : "Play"))
				{
					animator->m_Playing = !playing;
					animator->m_Scrubbing = !animator->m_Playing;
				}

				ImGui::SameLine();
				ImGui::AlignTextToFramePadding();
				ImGui::TextDisabled("%.2f / %.2f s", animator->m_Time, ClipDuration(*animator));
				ImGuiExt::EndPropertyRow();
			}

			/* 时间轴：拖到哪一帧就定格在哪一帧（播放随之停下） */
			{
				const float duration = ClipDuration(*animator);
				float time = animator->m_Time;
				const float value_width = ImGuiExt::BeginPropertyRow("Time");
				ImGui::SetNextItemWidth(value_width);
				if (duration > 1.0e-5f
					&& ImGui::SliderFloat("##AnimationTime", &time, 0.0f, duration, "%.2f s"))
				{
					animator->m_Playing = false;
					animator->m_Scrubbing = true;
					animator->m_Time = time;
				}
				ImGuiExt::EndPropertyRow();
			}

			ImGuiExt::DrawCheckboxUI("Loop", animator->m_Loop);

			/* 速度：与动画组件的属性卡同一口径 */
			{
				const float value_width = ImGuiExt::BeginPropertyRow("Speed");
				ImGui::SetNextItemWidth(value_width);
				ImGui::DragFloat("##AnimationSpeed", &animator->m_Speed,
					AnimatorComponent::kSpeedStep, AnimatorComponent::kSpeedMin,
					AnimatorComponent::kSpeedMax, "%.2fx");
				ImGuiExt::EndPropertyRow();
			}
		}

		PanelChrome::EndCard(card);
	}

	/* 预览实体上挂的动画组件（模型带片段时才挂） */
	AnimatorComponent* ModelEditorLayer::GetPreviewAnimator()
	{
		if (m_pDefaultScene == nullptr || !m_pDefaultScene->IsEntityValid(m_PreviewEntity))
			return nullptr;

		return m_pDefaultScene->GetRegistry().try_get<AnimatorComponent>(m_PreviewEntity);
	}

	/* 每个子模型一张卡：规模信息 + 它的材质（Shader 选择 + 按反射列出的参数行）。
	 * 悬停卡头 = 该子模型的网格段在预览里高亮（多个子网格时才做：单网格没有"哪一段"的歧义）。 */
	void ModelEditorLayer::ShowSubModelCards()
	{
		PROFILE_FUNCTION();

		const bool allow_highlight = m_pModelInfo->m_SubModelInfos.size() > 1;

		for (size_t i = 0; i < m_pModelInfo->m_SubModelInfos.size(); ++i)
		{
			const auto& sub_model_info = m_pModelInfo->m_SubModelInfos[i];
			const MaterialSlot* slot = m_pModel->GetSlotForSegment(i);
			if (sub_model_info == nullptr || slot == nullptr)
				continue;

			SharedPtr<Material> material =
				MaterialSlotEditing::Resolve(*m_pModel, m_SlotOverrides, slot->Name);
			if (material == nullptr)
				continue;

			ImGui::PushID(static_cast<int>(i));

			const PanelChrome::Card card = PanelChrome::BeginCard(
				sub_model_info->Name.empty() ? "Unnamed" : sub_model_info->Name.c_str(), Icons::Id::Model);

			if (allow_highlight && card.HeaderHovered)
				m_HighlightedSubModel = static_cast<int>(i);

			if (card.Open)
			{
				ImGuiExt::DrawCommonTextUI("AABB Min", FormatVec3(sub_model_info->AABB.first));
				ImGuiExt::DrawCommonTextUI("AABB Max", FormatVec3(sub_model_info->AABB.second));
				ImGuiExt::DrawCommonTextUI("Vertices",
					std::to_string(sub_model_info->VertexArray->GetVertexCount()));

				/* 材质区：来源行 + Shader + 参数行；编辑语义与属性面板共用 MaterialSlotEditing */
				ImGui::Separator();
				ImGui::TextColored(EditorTheme::Token::TextLabel, "Material Parameters");

				DrawSubModelMaterialRefRow(static_cast<int>(i));

				/* 换 Shader 先落到实例上：共享的模型默认 / 材质资产不能被就地改 */
				const std::string picked = MaterialParamUI::DrawMaterialShaderPicker(*material, m_ShaderOptions);
				if (!picked.empty())
				{
					bool changed = false;
					SharedPtr<Material> instance =
						MaterialSlotEditing::MakeInstance(*m_pModel, m_SlotOverrides, slot->Name, changed);
					if (instance != nullptr)
					{
						instance->SetShader(AssetManager::Instance().GetOrLoad<DeviceShader>(ABSOLUTE_PATH(picked)));
						material = std::move(instance);
						MarkSlotOverridesDirty();
					}
				}

				const std::vector<MaterialParamUI::MaterialParamRow> rows = MaterialParamUI::BuildMaterialParamRows(*material);
				if (rows.empty())
					MaterialParamUI::DrawMaterialReflectionNotice(*material);

				for (const MaterialParamUI::MaterialParamRow& row : rows)
				{
					MaterialParamUI::DrawMaterialParamRow(row.Param, row.Spec, m_AssetRevealFunc,
						[this, i](ParamType type, const std::string& name, const std::any& value)
						{
							WriteSubModelMaterialParam(i, type, name, value);
						});
				}

				/* 渲染队列：小于 2500 不透明，2500 起透明 */
				int32_t render_queue = material->GetRenderQueue();
				if (MaterialParamUI::DrawMaterialRenderQueueRow(render_queue))
				{
					bool queue_changed = false;
					MaterialSlotEditing::WriteRenderQueue(*m_pModel, m_SlotOverrides, slot->Name,
						render_queue, queue_changed);
					MarkSlotOverridesDirty();
				}
			}

			PanelChrome::EndCard(card);
			ImGui::PopID();
		}
	}


	/* 子模型卡的材质引用行：显示当前来源（覆盖来源 ?: 模型清单默认），拖 .mtl 落上来即挂载 */
	void ModelEditorLayer::DrawSubModelMaterialRefRow(int index)
	{
		const MaterialSlot* slot = m_pModel->GetSlotForSegment(static_cast<size_t>(index));
		if (slot == nullptr)
			return;

		const std::string source = MaterialSlotEditing::SourcePath(*m_pModel, m_SlotOverrides, slot->Name);
		const std::string display = source.empty()
			? std::string("(model default)")
			: PathToUtf8(PathFromUtf8(source).filename());

		const float value_width = ImGuiExt::BeginPropertyRow("Material", EditorTheme::Token::PropertyLabelWidth, true);

		/* 只读展示当前来源（文本可选中复制）；拖放目标与 tooltip 挂在输入框上 */
		char buffer[256] = {};
		snprintf(buffer, sizeof(buffer), "%s", display.c_str());
		ImGui::SetNextItemWidth(value_width);
		ImGui::InputText("##MaterialRef", buffer, sizeof(buffer), ImGuiInputTextFlags_ReadOnly);

		if (ImGui::IsItemHovered())
		{
			if (!source.empty())
				ImGui::SetTooltip("%s\n\nDrag a .mtl material asset here to assign or replace it. Editing parameters detaches from the asset.",
					RELATIVE_PATH(source).c_str());
			else
				ImGui::SetTooltip("Drag a .mtl material asset here to assign it");
		}

		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RESOURCE_BROWSER_ITEM"))
			{
				std::filesystem::path relative_path;
				if (payload->DataSize > 1 && TryPathFromUtf8Payload(
					payload->Data, static_cast<size_t>(payload->DataSize), relative_path))
				{
					const std::string extension = ToLowercase(PathToUtf8(relative_path.extension()).c_str());
					if (extension == ".mtl")
						AssignSubModelMaterial(static_cast<size_t>(index), PathToUtf8(g_AssetsPath / relative_path));
				}
			}
			ImGui::EndDragDropTarget();
		}

		ImGuiExt::EndPropertyRow();
	}

	SharedPtr<Material> ModelEditorLayer::GetSubModelMaterial(size_t index) const
	{
		const MaterialSlot* slot = m_pModel->GetSlotForSegment(index);
		if (slot == nullptr)
			return nullptr;

		return MaterialSlotEditing::Resolve(*m_pModel, m_SlotOverrides, slot->Name);
	}

	/* 当前模型第 index 段的材质来源（覆盖来源 ?: 清单默认；空 = 内嵌 / 白模） */
	std::string ModelEditorLayer::GetSubModelMaterialAssetPath(size_t index) const
	{
		const MaterialSlot* slot = m_pModel->GetSlotForSegment(index);
		if (slot == nullptr)
			return {};

		return MaterialSlotEditing::SourcePath(*m_pModel, m_SlotOverrides, slot->Name);
	}

	/* 挂材质资产：记成槽覆盖（共享引用）；改参数时由 MaterialSlotEditing 自动转实例 */
	bool ModelEditorLayer::AssignSubModelMaterial(size_t index, const std::string& absolute_path)
	{
		PROFILE_FUNCTION();

		const MaterialSlot* slot = m_pModel->GetSlotForSegment(index);
		if (slot == nullptr)
			return false;

		auto material = AssetManager::Instance().GetOrLoad<Material>(absolute_path);
		if (material == nullptr)
		{
			EDITOR_LOG_WARN("Failed to load material asset: {}.", absolute_path);
			return false;
		}

		MaterialSlotOverride slot_override;
		slot_override.pMaterial = std::move(material);
		slot_override.SourceAssetPath = absolute_path;
		m_SlotOverrides[slot->Name] = std::move(slot_override);
		MarkSlotOverridesDirty();
		return true;
	}

	/* 参数写回：与属性面板同一条路（改参数自动实例化） */
	void ModelEditorLayer::WriteSubModelMaterialParam(size_t index, ParamType type,
		const std::string& name, const std::any& value)
	{
		const MaterialSlot* slot = m_pModel->GetSlotForSegment(index);
		if (slot == nullptr)
			return;

		bool changed = false;
		MaterialSlotEditing::WriteParam(*m_pModel, m_SlotOverrides, slot->Name, type, name, value, changed);
		MarkSlotOverridesDirty();
	}

	/* 未导入模型时的提示：空窗口要说清"为什么是空的、该怎么办"。
	 * 按视口区域居中（在视口输入区之前画 —— 输入区占满整块，画在它之后文字会被挤出可见区） */
	void ModelEditorLayer::ShowEmptyModelHint(const ImVec2& area_min, const ImVec2& area_size)
	{
		PROFILE_FUNCTION();

		static constexpr const char* kHint = "Drag a model file into the viewport to open it";

		const float text_width = ImGui::CalcTextSize(kHint).x;

		ImGui::SetCursorScreenPos(ImVec2(
			area_min.x + ImMax(0.0f, (area_size.x - text_width) * 0.5f),
			area_min.y + area_size.y * 0.3f));

		ImGui::PushTextWrapPos(area_min.x + area_size.x);
		ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
		ImGui::TextUnformatted(kHint);
		ImGui::PopStyleColor();
		ImGui::PopTextWrapPos();
	}

	/* 响应拖拽文件到主窗口 */
	void ModelEditorLayer::OnDragItemToScene(const std::filesystem::path& path)
	{
		PROFILE_FUNCTION();

		LoadModelFile(PathToUtf8(path));
	}

	/* 导入模型：弹窗选文件 */
	void ModelEditorLayer::ImportModel()
	{
		PROFILE_FUNCTION();

		const auto file_path = FileDialog::OpenFile(
			"Model(*.mesh;*.obj;*.fbx;*.gltf;*.glb;*.dae;*.stl)\0*.mesh;*.obj;*.fbx;*.gltf;*.glb;*.dae;*.stl\0");
		if (file_path.empty())
			return;

		if (!LoadModelFile(file_path))
			return;

		/* 模型已就绪：层与视口窗口一并唤回，导入结果必然可见 */
		m_IsActivated = true;
		m_IsViewportVisible = true;
	}

	/* 打开模型（属性面板 / 资源面板入口） */
	void ModelEditorLayer::OpenModel(const std::string& path)
	{
		PROFILE_FUNCTION();

		if (!LoadModelFile(path))
			return;

		/* 模型已就绪：层与视口窗口一并唤回（同导入） */
		m_IsActivated = true;
		m_IsViewportVisible = true;
	}

	/* 打开模型文件：.mesh 直接加载；交换格式（obj / fbx / gltf / glb / dae / stl）先走内核
	 * 导入链转成 .mesh 再加载 —— 模型编辑器不自己解析格式，与「场景窗口拖入」共用同一条
	 * 转换链，材质与贴图一并带过来。转换落在资产目录的 Cache 工作区（不进资源树、可重复覆盖）。 */
	bool ModelEditorLayer::LoadModelFile(const std::string& path)
	{
		PROFILE_FUNCTION();

		if (path.empty())
			return false;

		const std::string extension = ToLowercase(PathToUtf8(PathFromUtf8(path).extension()).c_str());
		if (extension == ".model" || extension == ".mesh")
			return LoadMeshModel(path);

		const bool exchange_format = extension == ".obj" || extension == ".fbx" || extension == ".gltf"
			|| extension == ".glb" || extension == ".dae" || extension == ".stl";
		if (!exchange_format)
		{
			EDITOR_LOG_WARN("Model editor can not open this file: {}.", path);
			return false;
		}

		EDITOR_LOG_DEBUG("Import model file: {}.", path);

		const std::filesystem::path workbench =
			g_AssetsPath / "Cache" / "ModelEditor" / PathFromUtf8(path).stem();
		const std::string mesh_path = Helios::ImportModel(path, PathToUtf8(workbench));
		if (mesh_path.empty())
			return false;

		return LoadMeshModel(mesh_path);
	}

	/* 加载 .model / .mesh：模型编辑器对"场景里在用的那份资产"打开 —— 网格段与槽表都来自
	 * 资产本身；编辑落在槽覆盖上（改参数自动转实例），几何不重建。 */
	bool ModelEditorLayer::LoadMeshModel(const std::string& path)
	{
		PROFILE_FUNCTION();

		EDITOR_LOG_DEBUG("Open model asset: {}.", path);

		SharedPtr<Model> model = Model::Create(path);
		if (model == nullptr)
		{
			EDITOR_LOG_ERROR("Failed to load model: {}.", path);
			return false;
		}

		m_pModelInfo->Reset();
		m_pModelInfo->m_Path = path;
		m_SlotOverrides.clear();
		m_SlotOverridesDirty = false;
		m_ExportSlot = 0;

		/* 每个网格段一张子模型卡；材质按段的槽索引取（见 Model::GetSlotForSegment） */
		for (const auto& segment : model->GetMeshSegments())
		{
			auto sub_model_info = CreateSharedPtr<SubModelInfo>();
			sub_model_info->Name = segment->GetDebugName();
			sub_model_info->VertexArray = segment->GetMeshPrimitive().VertexArray;
			sub_model_info->AABB = std::make_pair(segment->GetAABBMin(), segment->GetAABBMax());
			m_pModelInfo->m_SubModelInfos.emplace_back(std::move(sub_model_info));
		}

		m_pModelInfo->m_AABB = std::make_pair(model->GetAABBMin(), model->GetAABBMax());

		/* 模型自带的动画片段就在 .mesh 旁边，一并收进来 */
		CollectAnimationClips(path);

		m_pModel = std::move(model);
		ShowModelInScene();
		return true;
	}

	/* 收集与 .mesh 同目录的动画片段：先按导入命名（<模型名>_<片段名>.anim）找，
	 * 找不到就认目录里所有 .anim —— 模型改过名的导出目录片段沿用原名，靠这条兜底。
	 * 按文件名排序，首个就是默认播的那个 */
	void ModelEditorLayer::CollectAnimationClips(const std::string& mesh_path)
	{
		PROFILE_FUNCTION();

		m_AnimationClips.clear();
		m_SelectedClip = -1;

		const std::filesystem::path path = PathFromUtf8(mesh_path);
		std::vector<std::filesystem::path> clip_paths =
			FindSiblingClipFiles(path, PathToUtf8(path.stem()) + "_");
		if (clip_paths.empty())
			clip_paths = FindSiblingClipFiles(path, {});

		for (const std::filesystem::path& clip_path : clip_paths)
		{
			auto clip = AssetManager::Instance().GetOrLoad<AnimationClip>(PathToUtf8(clip_path));
			if (clip != nullptr)
				m_AnimationClips.push_back(std::move(clip));
		}

		if (!m_AnimationClips.empty())
			m_SelectedClip = 0;
	}

	/* 一键保存模型（.model + .mesh + .mtl + .anim）：弹窗预填当前模型的文件名 */
	void ModelEditorLayer::ExportMeshAndMtl()
	{
		PROFILE_FUNCTION();

		if (!m_pModelInfo || !m_pModel)
			return;

		std::filesystem::path default_path = PathFromUtf8(m_pModelInfo->m_Path);
		default_path.replace_extension(".model");
		const auto file_path = FileDialog::SaveFile("Model(*.model)\0*.model\0",
			PathToUtf8(default_path.filename()).c_str());
		if (file_path.empty())
			return;

		SaveModelTo(file_path);
	}

	/* 保存到指定路径：几何原样拷成 .mesh，材质写成独立 .mtl，最后写 .model 清单组装。
	 * 内嵌 / 改过参数的条目生成 <stem>_materials/<Slot>.mtl；纯引用保留原路径。 */
	bool ModelEditorLayer::SaveModelTo(const std::string& model_file_path)
	{
		PROFILE_FUNCTION();

		if (!m_pModelInfo || !m_pModel)
			return false;

		const std::filesystem::path target_model = ResolveExportPath(model_file_path, ".model");
		if (target_model.empty())
			return false;

		const std::string stem = PathToUtf8(target_model.stem());
		const std::filesystem::path target_mesh = target_model.parent_path() / PathFromUtf8(stem + ".mesh");
		const std::filesystem::path source_mesh = PathFromUtf8(m_pModel->GetGeometryPath());
		if (source_mesh.empty())
			return false;

		std::error_code error;
		std::filesystem::create_directories(target_model.parent_path(), error);
		if (source_mesh != target_mesh)
		{
			std::filesystem::copy_file(source_mesh, target_mesh,
				std::filesystem::copy_options::overwrite_existing, error);
			if (error)
			{
				EDITOR_LOG_ERROR("Failed to export model geometry to {}: {}.",
					PathToUtf8(target_mesh), error.message());
				return false;
			}
		}

		/* 动画片段跟着模型走：拷到目标目录，文件名只留片段名 */
		for (const SharedPtr<AnimationClip>& clip : m_AnimationClips)
		{
			if (clip == nullptr || clip->GetPath().empty())
				continue;

			const std::filesystem::path clip_path = PathFromUtf8(clip->GetPath());
			const std::filesystem::path target_clip = target_model.parent_path()
				/ PathFromUtf8(ClipDisplayName(m_pModelInfo->m_Path, clip->GetPath()) + ".anim");
			if (target_clip == clip_path)
				continue;

			std::error_code clip_error;
			std::filesystem::copy_file(clip_path, target_clip,
				std::filesystem::copy_options::overwrite_existing, clip_error);
			if (clip_error)
			{
				EDITOR_LOG_WARN("Failed to save animation clip to {}: {}.",
					PathToUtf8(target_clip), clip_error.message());
			}
		}

		const std::filesystem::path materials_dir =
			target_model.parent_path() / PathFromUtf8(stem + "_materials");
		std::filesystem::create_directories(materials_dir, error);

		std::vector<ModelManifest::Slot> entries;
		std::set<std::string> used_material_names;
		const auto& slots = m_pModel->GetMaterialSlots();
		for (int index = 0; index < static_cast<int>(slots.size()); ++index)
		{
			const MaterialSlot& slot = slots[index];
			const auto override_iter = m_SlotOverrides.find(slot.Name);
			const MaterialSlotOverride* slot_override =
				(override_iter != m_SlotOverrides.end()) ? &override_iter->second : nullptr;

			const SharedPtr<Material> material = (slot_override != nullptr && slot_override->pMaterial != nullptr)
				? slot_override->pMaterial : slot.pDefault;
			if (material == nullptr)
				continue;

			/* 能沿用引用就沿用：实体覆盖是共享资产 / 模型默认本身是引用；
			 * 实例 / 内嵌 / 白模写成独立 .mtl */
			std::string asset_path;
			if (slot_override != nullptr && !slot_override->IsInstance
				&& !slot_override->SourceAssetPath.empty())
			{
				asset_path = slot_override->SourceAssetPath;
			}
			else if (slot_override == nullptr && !slot.SourcePath.empty())
			{
				asset_path = slot.SourcePath;
			}

			/* 交换格式导入的材质先落在工作区：里面是目标目录该有的那份材质，
			 * 导出时按内嵌重写，别让清单指回 Cache */
			if (IsScratchAssetPath(asset_path))
				asset_path.clear();

			if (asset_path.empty())
			{
				/* 文件名按槽名清理；清理后重名的加序号，别互相覆盖 */
				const std::string base_name = SanitizeFileName(slot.Name, "Slot_" + std::to_string(index));
				std::string slot_name = base_name;
				int suffix = 2;
				while (!used_material_names.insert(slot_name).second)
					slot_name = base_name + "_" + std::to_string(suffix++);

				const std::filesystem::path material_path = materials_dir / PathFromUtf8(slot_name + ".mtl");
				if (MaterialIO::WriteFile(PathToUtf8(material_path), *material))
				{
					/* 贴图跟着搬进目标目录，导出的模型自己就能加载 */
					MaterialIO::MakeTexturesLocal(PathToUtf8(material_path),
						PathToUtf8(target_model.parent_path() / "textures"));
				}
				else
				{
					EDITOR_LOG_WARN("Failed to export material: {}.", PathToUtf8(material_path));
				}
				asset_path = PathToUtf8(material_path);
			}

			ModelManifest::Slot entry;
			entry.Name = slot.Name;
			entry.AssetPath = asset_path;

			/* 模型默认自带的参数覆盖：没动过才原样写回（覆盖层属于引用形态） */
			if (slot_override == nullptr && slot.pOverride != nullptr && asset_path == slot.SourcePath)
			{
				entry.pOverride = slot.pOverride;
				entry.HasRasterOverride = slot.HasRasterOverride;
			}

			entries.push_back(std::move(entry));
		}

		if (!ModelManifest::Write(PathToUtf8(target_model), PathToUtf8(target_mesh), entries))
			return false;

		return true;
	}

	/* 单独导出模型：只拷 .mesh（骨骼段原样带走），不写材质与片段 */
	bool ModelEditorLayer::ExportModelTo(const std::string& mesh_file_path)
	{
		PROFILE_FUNCTION();

		if (!m_pModelInfo || !m_pModel)
			return false;

		const std::filesystem::path target_mesh = ResolveExportPath(mesh_file_path, ".mesh");
		const std::filesystem::path source_mesh = PathFromUtf8(m_pModel->GetGeometryPath());
		if (source_mesh.empty() || target_mesh.empty())
			return false;
		if (source_mesh == target_mesh)
			return true;

		std::error_code error;
		std::filesystem::copy_file(source_mesh, target_mesh,
			std::filesystem::copy_options::overwrite_existing, error);
		if (error)
		{
			EDITOR_LOG_ERROR("Failed to export model mesh to {}: {}.",
				PathToUtf8(target_mesh), error.message());
			return false;
		}
		return true;
	}

	/* 单独导出材质：只写选中槽的 .mtl（内嵌定义全量写） */
	bool ModelEditorLayer::ExportMaterialTo(const std::string& material_file_path, int slot_index)
	{
		PROFILE_FUNCTION();

		if (!m_pModelInfo || !m_pModel || m_pModel->GetMaterialSlots().empty())
			return false;

		const std::filesystem::path target = ResolveExportPath(material_file_path, ".mtl");
		if (target.empty())
			return false;

		const int index = std::clamp(slot_index, 0, static_cast<int>(m_pModel->GetMaterialSlots().size()) - 1);
		const MaterialSlot& slot = m_pModel->GetMaterialSlots()[static_cast<size_t>(index)];
		const SharedPtr<Material> material = MaterialSlotEditing::Resolve(*m_pModel, m_SlotOverrides, slot.Name);
		if (material == nullptr)
			return false;

		return MaterialIO::WriteFile(PathToUtf8(target), *material);
	}

	/* 单独导出动画：只写当前选中片段的 .anim（轨道与事件原样落盘） */
	bool ModelEditorLayer::ExportAnimationTo(const std::string& animation_file_path)
	{
		PROFILE_FUNCTION();

		if (m_SelectedClip < 0 || m_SelectedClip >= static_cast<int>(m_AnimationClips.size()))
			return false;

		const SharedPtr<AnimationClip>& clip = m_AnimationClips[static_cast<size_t>(m_SelectedClip)];
		if (clip == nullptr)
			return false;

		const std::filesystem::path target = ResolveExportPath(animation_file_path, ".anim");
		if (target.empty())
			return false;

		clip->Serializer(PathToUtf8(target));

		std::error_code error;
		return std::filesystem::exists(target, error);
	}

	/* 把当前模型放进编辑器场景：旧预览实体整棵销毁（骨骼的关节是它的子节点），
	 * 只留一个挂当前模型的实体；模型自带片段时自动挂动画组件并关联首个 clip，
	 * 关节树交给场景更新建好后开播 —— 打开就能看动。取景交给视口尺寸就绪的下一帧
	 * （m_AutoFocusPending → FocusOn，见 ShowModelWindowUI） */
	void ModelEditorLayer::ShowModelInScene()
	{
		PROFILE_FUNCTION();

		/* 先销毁上一个预览实体：Scene::DestroyEntity 会把骨骼模型的关节实体一并清掉。
		 * 只清组件的话关节树会留在场景里，换个模型就多存一棵。 */
		if (m_pDefaultScene->IsEntityValid(m_PreviewEntity))
		{
			Entity previous{ m_PreviewEntity, m_pDefaultScene->shared_from_this() };
			m_pDefaultScene->DestroyEntity(previous);
		}
		m_PreviewEntity = entt::null;

		if (m_pModel == nullptr)
			return;

		/* 将模型添加到场景中 */
		Entity entity = m_pDefaultScene->CreateEntity(m_pModel->GetDebugName());
		m_PreviewEntity = static_cast<entt::entity>(entity);

		auto& mesh_component = entity.AddComponent<ModelComponent>();
		mesh_component.m_Model = m_pModel;

		/* 预览实体用同一套槽覆盖：面板改参数 / 换贴图 / 换 Shader，预览下一帧跟着变 */
		mesh_component.m_SlotOverrides = m_SlotOverrides;
		m_SlotOverridesDirty = false;

		/* 模型自带动画：动画组件自动挂好，clip 关联首个；播放等场景更新把关节树建好再开
		 * （见 OnUpdate 的 m_AnimationStartPending，AutoPlay 也要那时才置位 ——
		 * 首帧就置位的话 Tick 会先跑，关节还不存在，绑不上就一直绑不上了） */
		m_AnimationStartPending = false;
		if (!m_AnimationClips.empty() && !m_pModel->IsStaticModel())
		{
			auto& animator = entity.AddComponent<AnimatorComponent>();
			animator.m_Clip = m_AnimationClips.front();
			animator.m_Loop = animator.m_Clip->IsLoop();
			animator.m_AutoPlay = false;
			animator.m_Playing = false;
			animator.m_Time = 0.0f;
			m_AnimationStartPending = true;
		}
		else
		{
			m_SelectedClip = -1;
		}

		/* 预览网格改成以模型为中心、格距按模型尺寸缩放（大模型取景距离远，场景口径的
		 * 1 格 1 单位网格罩不到模型）；取景本身等下一帧（m_AutoFocusPending） */
		const glm::vec3 aabb_min = m_pModel->GetAABBMin();
		const glm::vec3 aabb_max = m_pModel->GetAABBMax();
		const glm::vec3 aabb_center = (aabb_min + aabb_max) * 0.5f;
		const float aabb_radius = glm::length(aabb_max - aabb_min) * 0.5f;
		m_pEditorCamera->SetPreviewGrid(true, aabb_center, ChoosePreviewGridScale(aabb_radius));

		/* FocusOn 要用视口的宽高比（紧的一侧取景），等 UI 帧把尺寸落定后再聚焦 */
		m_AutoFocusPending = true;
	}
}
