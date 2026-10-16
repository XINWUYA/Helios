#include "Pch.h"
#include "ModelEditorLayer.h"
#include <glm/gtx/quaternion.hpp>
#include <limits>
#include "Helios/Application/Application.h"
#include "Helios/ImGui/EditorTheme.h"
#include "Helios/Scene/ModelImporter.h"
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
		m_pMaterialGroup = CreateSharedPtr<MaterialGroup>();

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
		m_pDefaultScene->OnUpdate(delta_time);

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

	/* 预览工具（左上角并排的三个图标按钮）：保存模型 / 线框预览 / 重置视图。
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
			false, "Save Model (.mesh + .mtl)"))
			ExportMeshAndMtl();
		hovered |= ImGui::IsItemHovered();

		/* 线框开着时按钮点亮（画面上只剩线框，按钮给个状态反馈） */
		ImGui::SameLine(0.0f, style.ItemSpacing.x);
		if (Icons::IconButton(Icons::Id::Grid, ImVec2(button_size, button_size),
			m_IsWireframeEnabled, "Wireframe"))
		{
			m_IsWireframeEnabled = !m_IsWireframeEnabled;
			m_pEditorCamera->SetWireframeOverlay(m_IsWireframeEnabled);
		}
		hovered |= ImGui::IsItemHovered();

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

	/* 每个子模型一张卡：规模信息 + 它的材质（Shader 选择 + 按反射列出的参数行）。
	 * 悬停卡头 = 该子模型的网格段在预览里高亮（多个子网格时才做：单网格没有"哪一段"的歧义）。 */
	void ModelEditorLayer::ShowSubModelCards()
	{
		PROFILE_FUNCTION();

		const bool allow_highlight = m_pModelInfo->m_SubModelInfos.size() > 1;

		for (size_t i = 0; i < m_pModelInfo->m_SubModelInfos.size(); ++i)
		{
			const auto& sub_model_info = m_pModelInfo->m_SubModelInfos[i];
			const auto& material = m_pMaterialGroup->GetMaterialByIndex(i);
			const MaterialEntry* entry = m_pMaterialGroup->GetEntryByIndex(static_cast<int>(i));
			if (sub_model_info == nullptr || material == nullptr || entry == nullptr)
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

				/* 材质区：来源引用行 + Shader 选择行 + 参数行（集合与类型都来自 Shader 反射，
				 * 换 Shader 行集跟着换；值缺的显示反射默认值）。与属性面板的材质卡共用同一套控件。 */
				ImGui::Separator();
				ImGui::TextColored(EditorTheme::Token::TextLabel, "Material Parameters");

				DrawSubModelMaterialRefRow(static_cast<int>(i), *entry);

				/* 换 Shader 也是编辑：挂着的资产引用脱钩（导出随条目形态走） */
				if (MaterialParamUI::DrawMaterialShaderRow(*material, m_ShaderOptions))
					DetachSubModelMaterialAsset(i);

				const std::vector<MaterialParamInfo> rows = MaterialParamUI::BuildMaterialParamRows(*material);
				if (rows.empty())
					MaterialParamUI::DrawMaterialReflectionNotice(*material);

				for (const MaterialParamInfo& param_info : rows)
				{
					/* 层里的材质就是编辑缓冲：改动落回材质本体（写回里顺带做脱钩） */
					MaterialParamUI::DrawMaterialParamRow(param_info, nullptr,
						[this, i](ParamType type, const std::string& name, const std::any& value)
						{
							WriteSubModelMaterialParam(i, type, name, value);
						});
				}
			}

			PanelChrome::EndCard(card);
			ImGui::PopID();
		}
	}

	/* 子模型卡的材质引用行：显示当前材质来源（资产引用 = 文件名；内嵌 = embedded），
	 * 拖一个 .mtl 落上来即挂载（或替换）—— 内容反序列化进编辑缓冲，见 AssignSubModelMaterial */
	void ModelEditorLayer::DrawSubModelMaterialRefRow(int index, const MaterialEntry& entry)
	{
		const bool by_asset = !entry.AssetPath.empty();
		const std::string display = by_asset
			? PathToUtf8(PathFromUtf8(entry.AssetPath).filename())
			: std::string("(embedded)");

		const float value_width = ImGuiExt::BeginPropertyRow("Material", EditorTheme::Token::PropertyLabelWidth, true);

		/* 只读展示当前来源（文本可选中复制）；拖放目标与 tooltip 挂在输入框上 */
		char buffer[256] = {};
		snprintf(buffer, sizeof(buffer), "%s", display.c_str());
		ImGui::SetNextItemWidth(value_width);
		ImGui::InputText("##MaterialRef", buffer, sizeof(buffer), ImGuiInputTextFlags_ReadOnly);

		if (ImGui::IsItemHovered())
		{
			if (by_asset)
				ImGui::SetTooltip("%s\n\nDrag a .mtl material asset here to assign or replace it. Editing parameters detaches from the asset.",
					RELATIVE_PATH(entry.AssetPath).c_str());
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

	/* 当前模型第 index 段的材质来源：资产引用路径（空 = 内嵌定义） */
	std::string ModelEditorLayer::GetSubModelMaterialAssetPath(size_t index) const
	{
		const MaterialEntry* entry = m_pMaterialGroup->GetEntryByIndex(static_cast<int>(index));
		return (entry != nullptr) ? entry->AssetPath : std::string();
	}

	/* 挂材质资产：GetOrLoad 把该 .mtl 反序列化（单条目、内嵌定义的材质资产），内容就地把
	 * 编辑缓冲那份材质覆盖成资产定义 —— 对象地址不变，预览与参数行立即跟随；条目记下
	 * 引用路径，导出随条目形态写引用（模型跟随材质资产变化）。 */
	bool ModelEditorLayer::AssignSubModelMaterial(size_t index, const std::string& absolute_path)
	{
		PROFILE_FUNCTION();

		const MaterialEntry* entry = m_pMaterialGroup->GetEntryByIndex(static_cast<int>(index));
		if (entry == nullptr || entry->pMaterial == nullptr)
			return false;

		auto material = AssetManager::Instance().GetOrLoad<Material>(absolute_path);
		if (material == nullptr)
		{
			EDITOR_LOG_WARN("Failed to load material asset: {}.", absolute_path);
			return false;
		}

		entry->pMaterial->CopyDefinitionFrom(*material);
		m_pMaterialGroup->SetEntryAssetPath(static_cast<int>(index), absolute_path);
		return true;
	}

	/* 参数写回：改动落编辑缓冲；挂着资产引用的随之转内嵌（导出写全量定义） */
	void ModelEditorLayer::WriteSubModelMaterialParam(size_t index, ParamType type,
		const std::string& name, const std::any& value)
	{
		const MaterialEntry* entry = m_pMaterialGroup->GetEntryByIndex(static_cast<int>(index));
		if (entry == nullptr || entry->pMaterial == nullptr)
			return;

		if (type == ParamType::Texture)
			entry->pMaterial->SetTexture(name, std::any_cast<SharedPtr<DeviceTexture>>(value));
		else
			entry->pMaterial->SetParameters(type, name, value);

		DetachSubModelMaterialAsset(index);
	}

	/* 材质被就地编辑：挂着的资产引用转内嵌 —— 改动塞进下一次导出的全量定义，
	 * 资产文件本身不动（改资产是资源面板里的事） */
	void ModelEditorLayer::DetachSubModelMaterialAsset(size_t index)
	{
		const MaterialEntry* entry = m_pMaterialGroup->GetEntryByIndex(static_cast<int>(index));
		if (entry != nullptr && !entry->AssetPath.empty())
			m_pMaterialGroup->SetEntryAssetPath(static_cast<int>(index), "");
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
		if (extension == ".mesh")
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

	/* 加载 .mesh：模型编辑器对"场景里在用的那份资产"打开 —— 网格段与槽表材质都来自资产本身。
	 * 材质克隆进编辑缓冲（就地改参数看效果），几何不重建、直接用加载好的对象。 */
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
		m_pMaterialGroup->ClearAllMaterials();

		/* 每个网格段一张子模型卡 + 一条材质条目（按段索引对位）：
		 * 材质取段的槽默认绑定（伴生 .mtl），没有就补默认材质 */
		const auto& segments = model->GetMeshSegments();
		for (size_t i = 0; i < segments.size(); ++i)
		{
			const auto& segment = segments[i];

			auto sub_model_info = CreateSharedPtr<SubModelInfo>();
			sub_model_info->Name = segment->GetDebugName();
			sub_model_info->VertexArray = segment->GetMeshPrimitive().VertexArray;
			sub_model_info->AABB = std::make_pair(segment->GetAABBMin(), segment->GetAABBMax());

			const MaterialSlot* slot = model->GetSlotByIndex(segment->GetSlotIndex());
			if (slot != nullptr)
				sub_model_info->MaterialParams.Name = slot->Name;
			m_pModelInfo->m_SubModelInfos.emplace_back(std::move(sub_model_info));

			auto material = (slot != nullptr && slot->pDefault != nullptr)
				? slot->pDefault->Clone()
				: Material::Create(AssetManager::Instance().GetOrLoad<DeviceShader>(
					ABSOLUTE_PATH("Shaders/MaterialShaders/PBRStandard.glsl")));
			m_pMaterialGroup->EmplaceMaterial(std::move(material));
			if (slot != nullptr)
			{
				m_pMaterialGroup->SetEntrySlotName(static_cast<int>(i), slot->Name);

				/* 槽默认材质来自资产引用（GetOrLoad 加载的带路径）：条目保留引用身份 ——
				 * 不改参数的话导出继续写引用形态（模型与材质资产保持同步） */
				if (slot->pDefault != nullptr && !slot->pDefault->GetPath().empty())
					m_pMaterialGroup->SetEntryAssetPath(static_cast<int>(i), slot->pDefault->GetPath());
			}
		}

		m_pModelInfo->m_AABB = std::make_pair(model->GetAABBMin(), model->GetAABBMax());

		m_pModel = std::move(model);
		ShowModelInScene();
		return true;
	}

	/* 拷贝指定格式的文件到目标路径 */
	static bool CopyFileFromTo(const std::filesystem::path& src_path, const std::filesystem::path& dst_path, const std::regex& suffix)
	{
		PROFILE_FUNCTION();

		if (src_path.empty() || src_path.empty())
			return false;

		if (!std::filesystem::exists(src_path))
			return false;

		for (auto& item : std::filesystem::directory_iterator(src_path))
		{
			std::filesystem::path dst_item_path = dst_path / item.path().filename();
			if (std::filesystem::is_directory(item.status()))
			{
				std::filesystem::create_directory(dst_item_path);
				CopyFileFromTo(item.path(), dst_item_path, suffix);
			}
			else
			{
				if (std::regex_match(item.path().extension().string(), suffix))
				{
					std::filesystem::copy_file(item.path(), dst_item_path, std::filesystem::copy_options::skip_existing);
				}
			}
		}
		return true;
	}

	/* 保存模型（.mesh & .mtl）：弹窗预填当前模型的文件名，选目标后走 SaveModelTo */
	void ModelEditorLayer::ExportMeshAndMtl()
	{
		PROFILE_FUNCTION();

		if (!m_pModelInfo || !m_pModel)
			return;

		const std::string default_name = PathToUtf8(PathFromUtf8(m_pModelInfo->m_Path).filename());
		const auto file_path = FileDialog::SaveFile("Mesh(*.mesh)\0*.mesh\0", default_name.c_str());
		if (file_path.empty())
			return;

		SaveModelTo(file_path);
	}

	/* 保存到指定路径：几何不可编辑 —— 当前 .mesh 原样拷贝到目标，再把材质与贴图写到同一
	 * 目录。材质条目按形态写出：内嵌的收敛参数后全量写、引用形态只写 Asset（模型跟随
	 * 材质资产）；贴图一并拷到目标目录（内嵌材质的引用是相对路径，导出目录自包含）。 */
	bool ModelEditorLayer::SaveModelTo(const std::string& mesh_file_path)
	{
		PROFILE_FUNCTION();

		if (!m_pModelInfo || !m_pModel)
			return false;

		/* 补后缀：对话框里没带扩展名（或给了别的）时按 .mesh 落盘 —— 模型只写这个格式 */
		std::string target_path = mesh_file_path;
		std::filesystem::path target_mesh = PathFromUtf8(target_path);
		if (ToLowercase(PathToUtf8(target_mesh.extension()).c_str()) != ".mesh")
		{
			target_mesh.replace_extension(".mesh");
			target_path = PathToUtf8(target_mesh);
		}

		const std::filesystem::path source_mesh = PathFromUtf8(m_pModelInfo->m_Path);
		if (source_mesh.empty() || target_mesh.empty())
			return false;

		/* 几何与打开的 .mesh 完全一致：原样拷贝（目标就是自己时跳过） */
		std::error_code error;
		if (source_mesh != target_mesh)
		{
			std::filesystem::copy_file(source_mesh, target_mesh,
				std::filesystem::copy_options::overwrite_existing, error);
			if (error)
			{
				EDITOR_LOG_ERROR("Failed to save model mesh to {}: {}.", target_path, error.message());
				return false;
			}
		}

		/* 导出材质：把导入时的材质名写进槽表（加载后覆盖表按槽名匹配）；
		 * 参数收敛只对内嵌条目做（反射是集合的单一来源，引用条目不写材质体） */
		for (size_t i = 0; i < m_pModelInfo->m_SubModelInfos.size(); ++i)
		{
			m_pMaterialGroup->SetEntrySlotName(static_cast<int>(i),
				m_pModelInfo->m_SubModelInfos[i]->MaterialParams.Name);
		}
		for (const MaterialEntry& entry : m_pMaterialGroup->GetEntries())
		{
			if (entry.AssetPath.empty() && entry.pMaterial != nullptr)
				entry.pMaterial->RetainShaderDeclaredParams();
		}

		std::filesystem::path target_mtl = target_mesh;
		target_mtl.replace_extension(".mtl");
		m_pMaterialGroup->Serializer(PathToUtf8(target_mtl));

		/* 拷贝贴图文件（材质里的引用是相对路径，导出目录要自包含） */
		const std::regex pattern(R"(^.*\.(pdf|png|jpeg|jpg|tga|bmp|dds)$)");
		CopyFileFromTo(source_mesh.parent_path(), target_mesh.parent_path(), pattern);

		return true;
	}

	/* 把当前模型放进编辑器场景：旧模型实体全部销毁，只留一个挂当前模型的实体；
	 * 取景交给视口尺寸就绪的下一帧（m_AutoFocusPending → FocusOn，见 ShowModelWindowUI） */
	void ModelEditorLayer::ShowModelInScene()
	{
		PROFILE_FUNCTION();

		/* 先移除场景中的其他模型 */
		m_pDefaultScene->DestroyTargetEntities<ModelComponent>();

		if (m_pModel == nullptr)
			return;

		/* 将模型添加到场景中 */
		Entity entity = m_pDefaultScene->CreateEntity(m_pModel->GetDebugName());
		auto& mesh_component = entity.AddComponent<ModelComponent>();
		mesh_component.m_Model = m_pModel;

		/* 编辑缓冲接到预览实体上：按段把槽覆盖指向 m_pMaterialGroup 里的材质 ——
		 * 面板改参数 / 换贴图 / 换 Shader 改的就是这一份，预览下一帧立刻可见；
		 * 导出写的也是它，两处同源（不接的话预览渲染的会是模型自带的默认材质，改什么都不会动） */
		for (size_t i = 0; i < m_pModelInfo->m_SubModelInfos.size(); ++i)
		{
			const std::string& slot_name = m_pModelInfo->m_SubModelInfos[i]->MaterialParams.Name;
			const auto& material = m_pMaterialGroup->GetMaterialByIndex(static_cast<int>(i));
			if (slot_name.empty() || material == nullptr)
				continue;

			MaterialSlotOverride& slot_override = mesh_component.m_SlotOverrides[slot_name];
			slot_override.pMaterial = material;
			slot_override.IsInstance = true;
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
