#include "Pch.h"
#include "SceneHierarchy.h"
#include "EditorCommon.h"
#include "EditorIcons.h"
#include "PanelChrome.h"
#include "PanelRegistry.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include "EntityTemplateRegistry.h"
#include "Command/ComponentFieldCommand.h"
#include "Command/AddComponentCommand.h"
#include "Command/RemoveComponentCommand.h"
#include "Command/CreateEntityCommand.h"
#include "Command/DeleteEntityCommand.h"
#include "Command/ReparentEntityCommand.h"
#include "Helios/ImGui/EditorTheme.h"
#include "Helios/Scene/Components.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace Helios
{
	namespace
	{
		/* 层级面板内部的拖拽载荷类型：被拖动实体的句柄 */
		constexpr const char* kEntityDragPayload = "SCENE_HIERARCHY_ENTITY";

		/* ---- 场景树节点图标 ----
		 * 按实体持有的组件判断它在树里"最像什么"，先匹配上的胜出。图标是编辑器表现层的东西，
		 * 映射就留在编辑器侧；以后新增可建实体类型时，在这补一条规则。 */

		bool HasCameraComponent(const Entity& entity)
		{
			return entity.HasComponent<CameraComponent>();
		}

		bool HasReflectionProbeComponent(const Entity& entity)
		{
			return entity.HasComponent<ReflectionProbeComponent>();
		}

		bool HasModelComponent(const Entity& entity)
		{
			return entity.HasComponent<ModelComponent>();
		}

		bool HasSpriteComponent(const Entity& entity)
		{
			return entity.HasComponent<SpriteComponent>();
		}

		/* 光源按类型再分：方向光 / 点光 / 聚光在树里要能一眼区分。
		 * 默认构造的 LightComponent 没有 Light 对象，这时退化看 m_Type。 */
		bool IsLightOfType(const Entity& entity, LightType type)
		{
			if (!entity.HasComponent<LightComponent>())
				return false;

			const auto& light_component = entity.GetComponent<LightComponent>();
			const LightType actual = (light_component.m_Light != nullptr)
				? light_component.m_Light->GetLightType()
				: light_component.m_Type;

			return actual == type;
		}

		bool IsDirectionalLight(const Entity& entity) { return IsLightOfType(entity, LightType::Directional); }
		bool IsSpotLight(const Entity& entity) { return IsLightOfType(entity, LightType::Spot); }
		/* 其余光源类型（Area / Volume）没有专门造型，沿用点光图标 */
		bool IsOtherLight(const Entity& entity) { return entity.HasComponent<LightComponent>(); }

		struct EntityIconRule
		{
			Icons::Id Icon;
			bool (*Match)(const Entity& entity);
		};

		constexpr EntityIconRule kEntityIconRules[] = {
			{ Icons::Id::Camera,           &HasCameraComponent },
			{ Icons::Id::LightDirectional, &IsDirectionalLight },
			{ Icons::Id::LightSpot,        &IsSpotLight },
			{ Icons::Id::LightPoint,       &IsOtherLight },
			{ Icons::Id::ReflectionProbe,  &HasReflectionProbeComponent },
			{ Icons::Id::Model,            &HasModelComponent },
			{ Icons::Id::Sprite,           &HasSpriteComponent },
		};

		/* 没有任何可识别组件的实体（只有 Name / Transform）用通用图标 */
		Icons::Id ResolveEntityIcon(const Entity& entity)
		{
			for (const EntityIconRule& rule : kEntityIconRules)
			{
				if (rule.Match != nullptr && rule.Match(entity))
					return rule.Icon;
			}

			return Icons::Id::Entity;
		}

		/* ---- 组件卡头部图标 ----
		 * 与实体图标同一套道理：图标是编辑器的表现层概念，注册表在 Kernel、不认识 Icons，
		 * 所以映射留在这里。按类型判定（不是按名字字符串），改显示名不影响图标。 */
		Icons::Id ResolveComponentIcon(const ComponentDesc& desc, const void* component)
		{
			const std::type_index& type = desc.Type;

			if (type == typeid(TransformComponent))       return Icons::Id::Transform;
			if (type == typeid(NameComponent))            return Icons::Id::Tag;
			if (type == typeid(CameraComponent))          return Icons::Id::Camera;
			if (type == typeid(ModelComponent))           return Icons::Id::Model;
			if (type == typeid(SpriteComponent))          return Icons::Id::Sprite;
			if (type == typeid(ReflectionProbeComponent)) return Icons::Id::ReflectionProbe;

			/* 光源是一个组件块但造型分三种：图标跟着「当前光源类型」走，
			 * 这样把 Type 从点光改成聚光时，卡头图标会立刻跟着变。
			 * 默认构造的 LightComponent 没有 Light 对象，这时退化看 m_Type。 */
			if (type == typeid(LightComponent) && component != nullptr)
			{
				const auto* light_component = static_cast<const LightComponent*>(component);
				const LightType light_type = (light_component->m_Light != nullptr)
					? light_component->m_Light->GetLightType()
					: light_component->m_Type;

				switch (light_type)
				{
				case LightType::Directional: return Icons::Id::LightDirectional;
				case LightType::Spot:        return Icons::Id::LightSpot;
				default:                     return Icons::Id::LightPoint;
				}
			}

			return Icons::Id::None;
		}

		/* 大小写无关的子串匹配（ToLowercase / ContainsCaseInsensitive）与树行内容
		 * （PanelChrome::DrawTreeRowLabel）都走 Editor 侧的共用实现，各面板语义一致。 */

		/* ---- 面板头部就地编辑的组件 ----
		 * Name（头部的实体名）和 Visibility（标题栏的眼睛开关）不单独成卡，就地放在属性面板顶部。
		 * 判定收口在这里：组件列表和头部两边共用。 */
		bool IsEditedInPanelHeader(const ComponentDesc& desc)
		{
			return desc.Type == typeid(NameComponent);
		}

		/* 把 std::string 喂给 InputText：缓冲区在栈上，改完写回；
		 * 返回是否被改动（调用方通常不关心，改动检测由 DrawEditableField 统一做）。 */
		bool InputTextString(const char* id, std::string& value, ImGuiInputTextFlags flags = 0)
		{
			char buffer[256] = {};
			std::strncpy(buffer, value.c_str(), sizeof(buffer) - 1);

			if (!ImGui::InputText(id, buffer, sizeof(buffer), flags))
				return false;

			value = buffer;
			return true;
		}

		/* 面板头部的实体名：无边框、标题字体 —— 看上去就是一行标题，但可以直接改。
		 * 悬停给一点底色、聚焦给控件底色，让「这里能编辑」看得出来。 */
		void DrawHeaderNameControl(const FieldDesc& field, void* field_ptr)
		{
			IM_UNUSED(field);

			const ImGuiStyle& style = ImGui::GetStyle();
			auto& name = *static_cast<std::string*>(field_ptr);

			/* 右侧给「添加组件」按钮让出位置，输入框才不会压到按钮上 */
			const float width = ImGui::GetContentRegionAvail().x
				- ImGui::GetFrameHeight() - style.ItemInnerSpacing.x;

			ImGui::PushFont(EditorTheme::GetFonts().Title);
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x * 0.5f, 2.0f));
			ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
			ImGui::PushStyleColor(ImGuiCol_FrameBg, EditorTheme::Token::Clear);
			ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
				EditorTheme::WithAlpha(EditorTheme::Token::Neutral5, 0.70f));
			ImGui::PushStyleColor(ImGuiCol_FrameBgActive, EditorTheme::Token::Neutral4);

			ImGui::SetNextItemWidth(ImMax(width, 40.0f));
			InputTextString("##EntityName", name);

			ImGui::PopStyleColor(3);
			ImGui::PopStyleVar(2);
			ImGui::PopFont();
		}

		/* 场景根节点的名字：取文件名（去掉 .scn 后缀），刚新建还没存过就是 Untitled */
		std::string MakeSceneDisplayName(const std::string& path)
		{
			if (path.empty())
				return "Untitled";

			return std::filesystem::path(path).stem().string();
		}

		/* 实体行内容：按实体持有的组件解析图标，再交给共用的树行版式 */
		void DrawEntityRowLabel(const Entity& entity, const std::string& name)
		{
			PanelChrome::DrawTreeRowLabel(ResolveEntityIcon(entity), name);
		}
	}

	SceneHierarchy::SceneHierarchy()
	{
		PROFILE_FUNCTION();
	}

	SceneHierarchy::SceneHierarchy(const SharedPtr<Scene>& scene)
	{
		PROFILE_FUNCTION();

		SetOwnerScene(scene);
	}

	void SceneHierarchy::SetOwnerScene(const SharedPtr<Scene>& scene)
	{
		m_pOwnerScene = scene;
		m_SelectedEntity = {};

		/* 场景已更换，字段编辑的合并窗口不再有效 */
		m_FieldEditTransactionOpen = false;

		/* 上一份场景里的挂接请求指向的句柄已经无效 */
		m_PendingReparent = {};
	}

	void SceneHierarchy::OnImGuiRender()
	{
		PROFILE_FUNCTION();

		/* 场景实体列表 */
		ShowSceneHierarchyUI();

		/* 实体属性 */
		ShowEntityPropertiesUI();
	}

	void SceneHierarchy::SetSelectedEntity(const Entity& entity)
	{
		m_SelectedEntity = entity;
	}

	void SceneHierarchy::ShowSceneHierarchyUI()
	{
		PROFILE_FUNCTION();

		/* 删除请求延后到遍历结束后执行：在 each 中销毁当前实体并不安全 */
		Entity pending_delete{};

		/* 层级关系每帧现算：子节点列表不作为第二份数据缓存 */
		EntityChildren children;
		std::vector<Entity> roots;
		BuildHierarchy(children, roots);

		/* 过滤时先算出"要显示的实体"：名字命中的 + 它们的全部祖先 */
		FilterSet visible;
		const FilterSet* filter = nullptr;
		if (m_EntityFilter[0] != '\0')
		{
			CollectFilteredEntities(visible);
			filter = &visible;
		}

		ImGui::Begin(Panel::kSceneHierarchy);
		{
			const int total_count = (m_pOwnerScene != nullptr)
				? static_cast<int>(m_pOwnerScene->GetRegistry().size()) : 0;
			ShowHierarchyHeader(filter != nullptr ? static_cast<int>(visible.size()) : total_count, total_count);

			/* 场景名作为根节点，实体树挂在它下面 */
			ShowSceneRootNode(roots, children, pending_delete, filter);

			/* 条目之下的空白区域：拖到这里表示提升到根层级。
			 * 只覆盖空白、不覆盖条目：落在条目上的拖拽目标因为"源与目标相同"会被
			 * ImGui 拒绝（拖起来又原位放下），这时若整窗也算目标，就会误当成提升到根层级。 */
			const ImVec2 window_pos = ImGui::GetWindowPos();
			const ImVec2 window_size = ImGui::GetWindowSize();
			const ImVec2 blank_top = ImGui::GetCursorScreenPos();
			const ImVec2 window_max{ window_pos.x + window_size.x, window_pos.y + window_size.y };

			if (window_max.y > blank_top.y + 1.0f)
			{
				const ImRect blank_area(blank_top, window_max);
				if (ImGui::BeginDragDropTargetCustom(blank_area, ImGui::GetID("##HierarchyRootDrop")))
				{
					if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityDragPayload))
					{
						const auto dragged = static_cast<entt::entity>(*static_cast<const uint32_t*>(payload->Data));
						m_PendingReparent = { dragged, entt::null };
					}

					ImGui::EndDragDropTarget();
				}
			}

			/* 拖到面板之外松手：默认挂到场景下，成为一级节点。
			 * 判据是"鼠标是否还在面板矩形内"，不能用"有没有拖拽目标接收"——
			 * 拖回原条目上松手时该条目的目标会被 ImGui 拒绝（源与目标相同），那不算拖到外面。 */
			const ImVec2 mouse_pos = ImGui::GetMousePos();
			const bool drop_inside_panel = (mouse_pos.x >= window_pos.x && mouse_pos.x < window_max.x
				&& mouse_pos.y >= window_pos.y && mouse_pos.y < window_max.y);
			if (!drop_inside_panel)
				ApplyDropOutsidePanel();

			/* 空白处右键，唤出新建（条目来自实体预设注册表） */
			if (ImGui::BeginPopupContextWindow("New", 1, false))
			{
				ShowCreateEntityMenu();
				ImGui::EndPopup();
			}
		}
		ImGui::End();

		/* 删除经命令栈落地，可撤销 */
		if (pending_delete)
			DeleteEntity(pending_delete);

		/* 挂接同样延后到这里：遍历中改层级会让本帧的子节点列表失真 */
		ApplyPendingReparent();
	}

	/* 「新建实体」菜单项：条目来自实体预设注册表，新增预设这里零改动 */
	void SceneHierarchy::ShowCreateEntityMenu()
	{
		PROFILE_FUNCTION();

		if (ImGui::BeginMenu("New A Entity"))
		{
			for (const EntityTemplateDesc& template_desc : EntityTemplateRegistry::Instance().All())
			{
				if (ImGui::MenuItem(template_desc.Name))
					m_SelectedEntity = CreateEntityFromTemplate(template_desc);
			}

			ImGui::EndMenu();
		}
	}

	/* 层级面板头部：过滤框 + 实体计数。
	 * 面板标题已经在 Tab 上，所以这里不再重复一遍场景名（同一条信息说两遍）。 */
	void SceneHierarchy::ShowHierarchyHeader(int shown_count, int total_count)
	{
		PROFILE_FUNCTION();

		const PanelChrome::HeaderRow header = PanelChrome::BeginHeaderRow(Icons::Id::None);
		const ImGuiStyle& style = ImGui::GetStyle();

		char count_text[32] = {};
		if (shown_count != total_count)
			snprintf(count_text, sizeof(count_text), "%d / %d", shown_count, total_count);
		else
			snprintf(count_text, sizeof(count_text), "%d", total_count);

		/* 计数贴右端：先量出文字宽度，剩下的才是过滤框的 */
		const float count_width = ImGui::CalcTextSize(count_text).x;
		const float row_width = header.Right - header.Min.x;
		const float filter_width = ImMax(row_width - count_width - style.ItemInnerSpacing.x * 2.0f, 60.0f);

		ImGui::SetNextItemWidth(filter_width);
		ImGui::InputTextWithHint("##HierarchyFilter", "Search entities...",
			m_EntityFilter, sizeof(m_EntityFilter));

		PanelChrome::PlaceHeaderAction(header, count_width);
		ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
		ImGui::TextUnformatted(count_text);
		ImGui::PopStyleColor();

		PanelChrome::EndHeaderRow(header);
	}

	/* 过滤词命中的实体 + 它们的全部祖先。
	 * 沿父链上溯即可：祖先链的终点是根实体，根节点由 ShowSceneRootNode 负责显示。 */
	void SceneHierarchy::CollectFilteredEntities(FilterSet& out) const
	{
		PROFILE_FUNCTION();

		out.clear();
		if (m_pOwnerScene == nullptr)
			return;

		const std::string needle = ToLowercase(m_EntityFilter);
		if (needle.empty())
			return;

		m_pOwnerScene->GetRegistry().each(
			[&](auto entity_id)
			{
				const Entity entity{ entity_id, m_pOwnerScene };
				if (!entity.HasComponent<NameComponent>())
					return;

				if (!ContainsCaseInsensitive(entity.GetComponent<NameComponent>().m_Name.c_str(), needle))
					return;

				/* 命中：自己与全部祖先都留下（父链断了的话命中的节点就没处挂） */
				entt::entity current = entity_id;
				for (int guard = 0; current != entt::null && guard < 64; ++guard)
				{
					if (!out.insert(current).second)
						break; /* 已经加过 → 这条父链的上面也都加过了 */

					current = m_pOwnerScene->GetParent(current);
				}
			});
	}

	/* 场景根节点：名字就是当前场景文件，实体树整体挂在它下面 */
	void SceneHierarchy::ShowSceneRootNode(const std::vector<Entity>& roots, const EntityChildren& children,
	                                       Entity& pending_delete, const FilterSet* filter)
	{
		PROFILE_FUNCTION();

		/* 用固定的 str_id：另存为换了名字时展开状态不该被重置（这个名字也不显示，
		 * 行内容由 PanelChrome::DrawTreeRowLabel 自绘） */
		const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
		const bool is_opened = ImGui::TreeNodeEx("##SceneRoot", flags);

		/* 场景自身不是实体：点它就把选中项清掉，属性面板随之空出来 */
		if (ImGui::IsItemClicked())
			m_SelectedEntity = {};

		/* 拖实体到场景上 = 提升到根层级（与拖到空白处等价，但更直观） */
		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityDragPayload))
			{
				const auto dragged = static_cast<entt::entity>(*static_cast<const uint32_t*>(payload->Data));
				m_PendingReparent = { dragged, entt::null };
			}

			ImGui::EndDragDropTarget();
		}

		/* 右键场景名也能新建实体：和空白处用同一个菜单 */
		if (ImGui::BeginPopupContextItem())
		{
			ShowCreateEntityMenu();
			ImGui::EndPopup();
		}

		/* 行内只显示文件名（够短），完整路径挂在悬停提示里；从未保存过就没有路径可显示 */
		if (!m_ScenePath.empty() && ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", m_ScenePath.c_str());

		PanelChrome::DrawTreeRowLabel(Icons::Id::Scene, MakeSceneDisplayName(m_ScenePath), m_SceneDirty);

		if (is_opened)
		{
			for (const Entity& root : roots)
				ShowEntityNode(root, children, pending_delete, filter);

			ImGui::TreePop();
		}
	}

	/* 一次遍历同时得到根节点与 父->子 列表 */
	void SceneHierarchy::BuildHierarchy(EntityChildren& children, std::vector<Entity>& roots) const
	{
		PROFILE_FUNCTION();

		children.clear();
		roots.clear();

		if (m_pOwnerScene == nullptr)
			return;

		m_pOwnerScene->GetRegistry().each(
			[&](auto entity_id)
			{
				Entity entity{ entity_id, m_pOwnerScene };

				const entt::entity parent = m_pOwnerScene->GetParent(entity_id);
				if (parent != entt::null)
					children[parent].emplace_back(entity);
				else
					roots.emplace_back(entity);
			});

		/* registry 的遍历顺序与创建顺序无关：按句柄升序排，顺序才稳定
		 * （与场景文件的输出顺序一致） */
		const auto by_handle = [](const Entity& lhs, const Entity& rhs)
		{
			return static_cast<uint32_t>(lhs) < static_cast<uint32_t>(rhs);
		};

		std::sort(roots.begin(), roots.end(), by_handle);
		for (auto& entry : children)
			std::sort(entry.second.begin(), entry.second.end(), by_handle);
	}

	/* 显示一个节点及其子树 */
	void SceneHierarchy::ShowEntityNode(const Entity& entity, const EntityChildren& children,
	                                    Entity& pending_delete, const FilterSet* filter)
	{
		PROFILE_FUNCTION();

		/* 过滤时：不在可见集合里的实体整棵子树都不画 */
		if (filter != nullptr && filter->find(entity) == filter->end())
			return;

		const auto& name = entity.GetComponent<NameComponent>().m_Name;

		const auto child_entry = children.find(entity);
		const std::vector<Entity>* sub_entities = (child_entry != children.end()) ? &child_entry->second : nullptr;
		const bool has_children = (sub_entities != nullptr && !sub_entities->empty());

		/* 过滤时全部展开：命中的实体挂在父链下面，父链不展开就等于没搜到 */
		if (filter != nullptr && has_children)
			ImGui::SetNextItemOpen(true, ImGuiCond_Always);

		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
		if (m_SelectedEntity == entity)
			flags |= ImGuiTreeNodeFlags_Selected;
		/* 没有子节点就是叶子：不能画成可展开的节点，否则会多出一层空节点 */
		if (!has_children)
			flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

		/* 空标签：节点自带的名字文字不画，改为下面自绘"图标 + 名字"。
		 * 必须传空字符串：格式化重载会显式传 label_end，而 ImGui 只在 label_end 为 NULL 时
		 * 才隐藏 "##" 之后的内容。 */
		const bool is_opened = ImGui::TreeNodeEx((void*)(uint64_t)(uint32_t)entity, flags, "%s", "");

		if (ImGui::IsItemClicked())
		{
			m_SelectedEntity = entity;
		}

		/* 拖拽源：把这个实体挂到别的节点下 */
		if (ImGui::BeginDragDropSource())
		{
			const uint32_t handle = static_cast<uint32_t>(entity);
			ImGui::SetDragDropPayload(kEntityDragPayload, &handle, sizeof(handle));
			ImGui::TextUnformatted(name.c_str());
			ImGui::EndDragDropSource();
		}

		/* 拖拽目标：接收别的实体做自己的子节点。
		 * 落到自己或自己的后代上会成环，这种情况直接不接受 */
		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityDragPayload))
			{
				const auto dragged = static_cast<entt::entity>(*static_cast<const uint32_t*>(payload->Data));
				if (m_pOwnerScene != nullptr && !m_pOwnerScene->IsAncestor(dragged, entity))
					m_PendingReparent = { dragged, static_cast<entt::entity>(entity) };
			}

			ImGui::EndDragDropTarget();
		}

		/* 右键选择删除当前节点 */
		if (ImGui::BeginPopupContextItem())
		{
			if (ImGui::MenuItem("Delete"))
				pending_delete = entity;

			ImGui::EndPopup();
		}

		/* 交互都在上面处理完了（那时"上一项"才是树节点），这里补画行内容 */
		DrawEntityRowLabel(entity, name);

		/* 展开子节点（叶子节点没有 TreePush，无需 TreePop） */
		if (has_children && is_opened)
		{
			for (const Entity& child : *sub_entities)
				ShowEntityNode(child, children, pending_delete, filter);

			ImGui::TreePop();
		}
	}

	/* 拖到面板之外松手 = 挂到场景下（成为一级节点）。
	 * 只在"确实在拖实体载荷 + 这一帧松手"时生效；路径与变换的处理都复用挂接流程。 */
	void SceneHierarchy::ApplyDropOutsidePanel()
	{
		if (m_pOwnerScene == nullptr || !ImGui::IsMouseReleased(ImGuiMouseButton_Left))
			return;

		const ImGuiPayload* payload = ImGui::GetDragDropPayload();
		if (payload == nullptr || !payload->IsDataType(kEntityDragPayload))
			return;

		const auto dragged = static_cast<entt::entity>(*static_cast<const uint32_t*>(payload->Data));
		m_PendingReparent = { dragged, entt::null };
	}

	void SceneHierarchy::ApplyPendingReparent()
	{
		if (m_PendingReparent.Child == entt::null || m_pOwnerScene == nullptr)
			return;

		const PendingReparent request = m_PendingReparent;
		m_PendingReparent = {};

		ReparentEntity(Entity{ request.Child, m_pOwnerScene }, request.Parent);
	}

	/* 改变父节点：父子关系与本地变换一起改（保持世界位置），有命令栈时可撤销 */
	void SceneHierarchy::ReparentEntity(Entity entity, entt::entity parent)
	{
		PROFILE_FUNCTION();

		if (m_pOwnerScene == nullptr || !m_pOwnerScene->IsEntityValid(entity))
			return;

		const entt::entity before_parent = m_pOwnerScene->GetParent(entity);
		if (before_parent == parent)
			return;

		const bool has_transform = entity.HasComponent<TransformComponent>();
		TransformComponent before;
		if (has_transform)
			before = entity.GetComponent<TransformComponent>();

		/* 换父空间会重算本地变换，命令要连它一起记录才能原样撤销 */
		if (!m_pOwnerScene->SetParent(entity, parent))
			return;

		if (m_pCommandStack == nullptr)
			return;

		TransformComponent after = before;
		if (has_transform)
			after = entity.GetComponent<TransformComponent>();

		m_pCommandStack->Execute(CreateUniquePtr<ReparentEntityCommand>(
			m_pOwnerScene, entity, before_parent, before, parent, after));
	}

	/* 新建实体：走命令栈则实体本身也进历史（撤销即消失） */
	Entity SceneHierarchy::CreateEntityFromTemplate(const EntityTemplateDesc& template_desc)
	{
		if (m_pOwnerScene == nullptr)
			return {};

		if (m_pCommandStack == nullptr)
		{
			Entity entity = m_pOwnerScene->CreateEntity(template_desc.Name);
			for (const AddFunc add : template_desc.Components)
			{
				if (add != nullptr)
					add(entity);
			}
			return entity;
		}

		auto command = CreateUniquePtr<CreateEntityCommand>(m_pOwnerScene, template_desc.Name, template_desc.Components);
		CreateEntityCommand* raw = command.get();
		m_pCommandStack->Execute(std::move(command));
		return raw->GetEntity();
	}

	void SceneHierarchy::DeleteEntity(Entity entity)
	{
		if (!entity || m_pOwnerScene == nullptr)
			return;

		if (m_pCommandStack == nullptr)
		{
			m_pOwnerScene->DestroyEntity(entity);
		}
		else
		{
			m_pCommandStack->Execute(CreateUniquePtr<DeleteEntityCommand>(m_pOwnerScene, entity));
		}

		if (m_SelectedEntity == entity)
			m_SelectedEntity = {};
	}

	void SceneHierarchy::ShowEntityPropertiesUI()
	{
		PROFILE_FUNCTION();

		ImGui::Begin(Panel::kProperties);
		{
			if (m_SelectedEntity)
			{
				ShowPropertiesHeader();
				ShowEntityComponents();
			}
			else
			{
				ShowEmptyPropertiesHint();
			}
		}
		ImGui::End();
	}

	/* 面板头部：实体图标 + 实体名（就地可编辑）+ 右侧「添加组件」。
	 * 头部自带上下文 —— 不必回头看层级树才知道正在编辑哪个实体；
	 * 实体名同时也就不再单独占一张 Name 组件卡（同一条信息只说一次）。 */
	void SceneHierarchy::ShowPropertiesHeader()
	{
		PROFILE_FUNCTION();

		const PanelChrome::HeaderRow header = PanelChrome::BeginHeaderRow(ResolveEntityIcon(m_SelectedEntity));

		/* 实体名：就地编辑，与组件字段走同一条命令路径（可撤销）。
		 * 头部只放得下一行，所以取该组件的第一个文本字段 —— 头部就地编辑的组件
		 * 必须只有一个文本字段（见 IsEditedInPanelHeader）。 */
		for (const ComponentDesc& desc : ComponentRegistry::Instance().All())
		{
			if (!IsEditedInPanelHeader(desc) || desc.GetPtr == nullptr)
				continue;

			void* component = desc.GetPtr(m_SelectedEntity);
			if (component == nullptr)
				break;

			for (const FieldDesc& field : desc.Fields)
			{
				if (field.Type != FieldType::String)
					continue;

				DrawEditableField(desc, m_SelectedEntity, component, field, &DrawHeaderNameControl);
				break;
			}

			break;
		}

		/* 添加组件入口贴右端，与头部同一行 */
		const float button_size = ImGui::GetFrameHeight();
		PanelChrome::PlaceHeaderAction(header, button_size);
		ShowAddComponentButton();

		PanelChrome::EndHeaderRow(header);
	}

	/* 没有选中实体时的提示：空面板要说清"为什么是空的、该怎么办" */
	void SceneHierarchy::ShowEmptyPropertiesHint()
	{
		PROFILE_FUNCTION();

		static constexpr const char* kHint = "Select an entity to edit its components";

		const float wrap_width = ImGui::GetContentRegionAvail().x;
		const float text_width = ImGui::CalcTextSize(kHint).x;

		/* 垂直方向约三分之一处：视线自然会落到这里，不像顶部一行那么容易被忽略 */
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetContentRegionAvail().y * 0.3f));

		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap_width);
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImMax(0.0f, (wrap_width - text_width) * 0.5f));
		ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
		ImGui::TextUnformatted(kHint);
		ImGui::PopStyleColor();
		ImGui::PopTextWrapPos();
	}

	/* 按字段类型绘制单个控件，field_ptr 指向可写的字段存储 */
	static void DrawFieldControl(const FieldDesc& field, void* field_ptr)
	{
		switch (field.Type)
		{
		case FieldType::Bool:
			ImGuiExt::DrawCheckboxUI(field.Name, *reinterpret_cast<bool*>(field_ptr));
			break;
		case FieldType::Int:
			ImGuiExt::DrawDragIntUI(field.Name, *reinterpret_cast<int*>(field_ptr));
			break;
		case FieldType::Float:
			ImGuiExt::DrawDragFloatUI(field.Name, *reinterpret_cast<float*>(field_ptr));
			break;
		case FieldType::Vec2:
			ImGuiExt::DrawDragFloat2UI(field.Name, *reinterpret_cast<glm::vec2*>(field_ptr));
			break;
		case FieldType::Vec3:
		{
			auto& vec = *reinterpret_cast<glm::vec3*>(field_ptr);
			if (field.Semantics != nullptr && std::strcmp(field.Semantics, "AngleDeg") == 0)
			{
				/* 内部以弧度存储、Inspector 按角度编辑 */
				glm::vec3 degree = glm::degrees(vec);
				ImGuiExt::DrawVec3ControlUI(field.Name, degree, field.ResetValue);
				vec = glm::radians(degree);
			}
			else
			{
				ImGuiExt::DrawVec3ControlUI(field.Name, vec, field.ResetValue);
			}
			break;
		}
		case FieldType::Vec4:
		case FieldType::Color:
			ImGuiExt::DrawColorUI(field.Name, *reinterpret_cast<glm::vec4*>(field_ptr));
			break;
		case FieldType::Enum:
		{
			if (field.GetEnumNames == nullptr)
				break;

			/* 枚举底层类型可能小于 int（如 uint8_t），按实际大小读写 */
			const size_t size = (field.ValueSize == 0 || field.ValueSize > sizeof(int))
				? sizeof(int) : field.ValueSize;
			int value = 0;
			std::memcpy(&value, field_ptr, size);
			ImGuiExt::DrawComboUI(field.Name, field.GetEnumNames(), value);
			std::memcpy(field_ptr, &value, size);
			break;
		}
		case FieldType::String:
		{
			/* 文本行也走统一的属性行布局，标签与其它行同宽同色 */
			const float value_width = ImGuiExt::BeginPropertyRow(field.Name);
			ImGui::SetNextItemWidth(value_width);
			InputTextString("##value", *reinterpret_cast<std::string*>(field_ptr));
			ImGuiExt::EndPropertyRow();
			break;
		}
		default:
			break;
		}
	}

	namespace
	{
		/* 字段值的字节数（定长字段由注册表给出） */
		size_t FieldByteSize(const FieldDesc& field)
		{
			return (field.ValueSize != 0) ? field.ValueSize : sizeof(float);
		}

		/* 访问器字段经 getter/setter 读写，偏移字段直接取址 */
		bool IsAccessorField(const FieldDesc& field)
		{
			return field.Get != nullptr && field.Set != nullptr;
		}
	}

	/* 单个字段的编辑：取快照 → 画控件 → 比较 → 有变化就生成历史命令。组件卡的字段和面板
	 * 头部的实体名都走这里 —— 撤销语义只有这一份实现（名字连改十次也只留一条历史）。 */
	void SceneHierarchy::DrawEditableField(const ComponentDesc& desc, Entity& entity, void* component,
	                                       const FieldDesc& field, FieldDrawFunc draw)
	{
		/* 上一帧有控件活跃、这一帧没有 → 一次编辑结束，给命令栈封口。
		 * 否则下一次编辑会并进上一条历史，撤销一次退过头。 */
		const bool item_active = ImGui::IsAnyItemActive();
		if (m_FieldEditTransactionOpen && !item_active)
		{
			m_FieldEditTransactionOpen = false;
			if (m_pCommandStack != nullptr)
				m_pCommandStack->EndTransaction();
		}

		const size_t value_size = FieldByteSize(field);
		const bool is_text = (field.Type == FieldType::String);
		const bool is_accessor = IsAccessorField(field);

		/* 编辑前的值，以及供控件操作的存储 */
		alignas(16) uint8_t before[kFieldValueCapacity] = {};
		alignas(16) uint8_t edited[kFieldValueCapacity] = {};
		std::string before_text;

		void* field_ptr = nullptr;
		if (is_accessor)
		{
			field.Get(component, edited);
			std::memcpy(before, edited, value_size);
			field_ptr = edited;
		}
		else
		{
			field_ptr = static_cast<uint8_t*>(component) + field.Offset;
			if (is_text)
				before_text = *static_cast<const std::string*>(field_ptr);
			else
				std::memcpy(before, field_ptr, value_size);
		}

		draw(field, field_ptr);

		/* 访问器字段：把编辑结果写回对象 */
		if (is_accessor)
			field.Set(component, field_ptr);

		/* 只有用户正在操作控件时才生成命令：外部改动（如 Gizmo 拖拽、
		 * 组件被移除）不是字段编辑，不应产生历史 */
		if (m_pCommandStack == nullptr || !ImGui::IsAnyItemActive())
			return;

		const bool changed = is_text
			? (before_text != *static_cast<const std::string*>(field_ptr))
			: (std::memcmp(before, field_ptr, value_size) != 0);
		if (!changed)
			return;

		if (!m_FieldEditTransactionOpen)
		{
			m_FieldEditTransactionOpen = true;
			m_pCommandStack->BeginTransaction();
		}

		if (is_text)
		{
			m_pCommandStack->Execute(CreateUniquePtr<ComponentFieldCommand>(
				entity, desc, field, before_text, *static_cast<const std::string*>(field_ptr)));
		}
		else
		{
			m_pCommandStack->Execute(CreateUniquePtr<ComponentFieldCommand>(
				entity, desc, field, before, field_ptr));
		}
	}

	/* 按字段元数据生成控件（组件卡的卡身） */
	void SceneHierarchy::DrawComponentFieldsBySchema(const ComponentDesc& desc, Entity& entity, void* component)
	{
		for (const FieldDesc& field : desc.Fields)
		{
			/* 条件不满足：不画该行（字段值保留，序列化照常） */
			if (!EvaluateCondition(field.Condition, component))
				continue;

			ImGui::PushID(field.Name);
			DrawEditableField(desc, entity, component, field, &DrawFieldControl);
			ImGui::PopID();
		}
	}

	/* 绘制单个组件块：卡头（折叠箭头 + 图标 + 组件名 + 右侧菜单）+ 卡身（schema 字段 +
	 * 组件自定义绘制）。卡片版式来自 PanelChrome，各面板共用。 */
	void SceneHierarchy::DrawComponentBlock(const ComponentDesc& desc, Entity& entity, void* component)
	{
		PROFILE_FUNCTION();

		/* 外层已 PushID(组件类型)，卡片的展开状态键因此天然按组件分开 */
		const PanelChrome::Card card = PanelChrome::BeginCard(desc.Name,
			ResolveComponentIcon(desc, component), Icons::Id::Menu, "Component options");

		if (card.ActionClicked)
			ImGui::OpenPopup("ComponentSettings");

		if (card.Open)
		{
			DrawComponentFieldsBySchema(desc, entity, component);
			if (desc.CustomDraw != nullptr)
				desc.CustomDraw(component);
		}

		PanelChrome::EndCard(card);

		/* 删除组件：经命令栈则可连组件数据一起还原 */
		if (ImGui::BeginPopup("ComponentSettings"))
		{
			if (ImGui::MenuItem("Remove") && desc.Remove != nullptr)
			{
				if (m_pCommandStack != nullptr)
				{
					m_pCommandStack->Execute(CreateUniquePtr<RemoveComponentCommand>(entity, desc));
				}
				else
				{
					desc.Remove(entity);
				}
			}

			ImGui::EndPopup();
		}
	}

	void SceneHierarchy::ShowEntityComponents()
	{
		PROFILE_FUNCTION();

		/* 全部组件：由 ComponentRegistry 驱动，本类不认识任何具体组件类型。
		 * 新增组件只需在注册表补一段，这里零改动。
		 * 例外只有一处：已在面板头部就地编辑的组件（Name）不再单独成卡。 */
		for (const ComponentDesc& desc : ComponentRegistry::Instance().All())
		{
			if (IsEditedInPanelHeader(desc))
				continue;

			if (desc.Has == nullptr || !desc.Has(m_SelectedEntity))
				continue;

			void* component = (desc.GetPtr != nullptr) ? desc.GetPtr(m_SelectedEntity) : nullptr;
			if (component == nullptr)
				continue;

			/* 组件级条件：如持有对象为空则整块不显示 */
			if (desc.Visible != nullptr && !desc.Visible(component))
				continue;

			ImGui::PushID(static_cast<int>(desc.Type.hash_code()));
			DrawComponentBlock(desc, m_SelectedEntity, component);
			ImGui::PopID();
		}
	}

	/* 增加组件按钮：菜单项来自注册表（bAddable == false 的组件不出现，如 Name / Transform）。
	 * 组件一多，菜单很难找 —— 顶部给一个过滤框，打开即自动聚焦。 */
	void SceneHierarchy::ShowAddComponentButton()
	{
		PROFILE_FUNCTION();

		const float button_size = ImGui::GetFrameHeight();
		if (Icons::IconButton(Icons::Id::Add, ImVec2(button_size, button_size), false, "Add Component"))
			ImGui::OpenPopup("AddComponentPopup");

		if (!ImGui::BeginPopup("AddComponentPopup"))
			return;

		if (ImGui::IsWindowAppearing())
		{
			m_AddComponentFilter[0] = '\0';
			ImGui::SetKeyboardFocusHere();
		}

		ImGui::SetNextItemWidth(220.0f);
		ImGui::InputTextWithHint("##AddComponentSearch", "Search...", m_AddComponentFilter,
			sizeof(m_AddComponentFilter));
		ImGui::Separator();

		const std::string needle = ToLowercase(m_AddComponentFilter);
		bool any_match = false;

		for (const ComponentDesc& desc : ComponentRegistry::Instance().All())
		{
			if (!desc.bAddable)
				continue;

			for (const ComponentVariant& variant : desc.Variants)
			{
				if (variant.Add == nullptr)
					continue;

				const char* label = (variant.MenuName != nullptr) ? variant.MenuName : desc.Name;
				if (!ContainsCaseInsensitive(label, needle))
					continue;

				any_match = true;

				if (ImGui::MenuItem(label))
				{
					if (m_pCommandStack != nullptr)
					{
						m_pCommandStack->Execute(CreateUniquePtr<AddComponentCommand>(
							m_SelectedEntity, desc.Name, variant.Add, desc.Remove));
					}
					else
					{
						variant.Add(m_SelectedEntity);
					}

					ImGui::CloseCurrentPopup();
				}
			}
		}

		if (!any_match)
			ImGui::TextDisabled("No matching component");

		ImGui::EndPopup();
	}
}
