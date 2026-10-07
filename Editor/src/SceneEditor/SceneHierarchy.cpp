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

		class ComponentCustomDrawCommand final : public ICommand
		{
		public:
			ComponentCustomDrawCommand(Entity entity, HasFunc has, ComponentRestoreFunc restore,
				ComponentSnapshot before, ComponentSnapshot after, const char* component_name)
				: m_Entity(entity), m_Has(has), m_Restore(restore),
				  m_Before(std::move(before)), m_After(std::move(after)),
				  m_Label(std::string("Edit ") + (component_name != nullptr ? component_name : "Component"))
			{
			}

			void Do() override { Apply(m_After); }
			void Undo() override { Apply(m_Before); }
			[[nodiscard]] const char* GetLabel() const override { return m_Label.c_str(); }

		private:
			void Apply(const ComponentSnapshot& snapshot)
			{
				if (m_Has != nullptr && m_Has(m_Entity) && m_Restore != nullptr && snapshot.IsValid())
					m_Restore(m_Entity, snapshot);
			}

			Entity m_Entity;
			HasFunc m_Has{ nullptr };
			ComponentRestoreFunc m_Restore{ nullptr };
			ComponentSnapshot m_Before;
			ComponentSnapshot m_After;
			std::string m_Label;
		};

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

		/* 内建形状实体（默认 Cube / Sphere / Plane）：带模型、且模型是内建几何时，
		 * 树里显示各自的形状图标（排在最泛化的 Model 规则之前，先匹配者胜）。 */
		bool IsBuiltinShape(const Entity& entity, BuiltinModelType type)
		{
			if (!entity.HasComponent<ModelComponent>())
				return false;

			const auto& model = entity.GetComponent<ModelComponent>().m_Model;
			BuiltinModelType actual{};
			return model != nullptr && model->TryGetBuiltinType(actual) && actual == type;
		}

		bool IsBuiltinCube(const Entity& entity) { return IsBuiltinShape(entity, BuiltinModelType::Cube); }
		bool IsBuiltinSphere(const Entity& entity) { return IsBuiltinShape(entity, BuiltinModelType::Sphere); }
		bool IsBuiltinPlane(const Entity& entity) { return IsBuiltinShape(entity, BuiltinModelType::Plane); }

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
			{ Icons::Id::Cube,             &IsBuiltinCube },
			{ Icons::Id::Sphere,           &IsBuiltinSphere },
			{ Icons::Id::Plane,            &IsBuiltinPlane },
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

		/* ---- 「新建」菜单的分组视图（顶栏 / 右键共用）----
		 * 按 Category 折叠：没分组的条目各自成节，同组的在首项注册的位置聚成一节（组内保持注册
		 * 顺序）；组行图标按组名集中映射（组只是展示概念，不进注册表）。 */
		struct EntityMenuEntry
		{
			const char* Group{ nullptr };                       /* nullptr = 未分组的单项 */
			const EntityTemplateDesc* Single{ nullptr };        /* Group == nullptr 时有效 */
			std::vector<const EntityTemplateDesc*> Members;     /* Group != nullptr 时有效 */
		};

		Icons::Id CategoryIconOf(const char* category)
		{
			if (std::strcmp(category, "3D") == 0)
				return Icons::Id::Shape3D;
			if (std::strcmp(category, "Light") == 0)
				return Icons::Id::Light;

			return Icons::Id::Entity;
		}

		std::vector<EntityMenuEntry> BuildEntityMenuEntries(const std::vector<EntityTemplateDesc>& templates)
		{
			std::vector<EntityMenuEntry> entries;

			for (const EntityTemplateDesc& desc : templates)
			{
				if (desc.Category == nullptr)
				{
					EntityMenuEntry single;
					single.Single = &desc;
					entries.push_back(std::move(single));
					continue;
				}

				/* 组已出现过就跳过：成员在首项处一次性收集 */
				bool seen = false;
				for (const EntityMenuEntry& entry : entries)
				{
					if (entry.Group != nullptr && std::strcmp(entry.Group, desc.Category) == 0)
					{
						seen = true;
						break;
					}
				}
				if (seen)
					continue;

				EntityMenuEntry group;
				group.Group = desc.Category;
				for (const EntityTemplateDesc& member : templates)
				{
					if (member.Category != nullptr && std::strcmp(member.Category, desc.Category) == 0)
						group.Members.push_back(&member);
				}
				entries.push_back(std::move(group));
			}

			return entries;
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

		/* 「添加组件」菜单条目的图标：光源的三种形态按注册的 Variant 名判断（lambda 里拿不到
		 * 类型信息）—— 改名的时候这里要跟着改；其余组件按类型判断（跟组件卡同一套）。 */
		Icons::Id ResolveComponentMenuIcon(const ComponentDesc& desc, const ComponentVariant& variant)
		{
			if (desc.Type == typeid(LightComponent) && variant.MenuName != nullptr)
			{
				if (std::strcmp(variant.MenuName, "Directional Light") == 0) return Icons::Id::LightDirectional;
				if (std::strcmp(variant.MenuName, "Spot Light") == 0)        return Icons::Id::LightSpot;
				if (std::strcmp(variant.MenuName, "Point Light") == 0)       return Icons::Id::LightPoint;
			}

			return ResolveComponentIcon(desc, nullptr);
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

		/* 栏与主体之间那条分隔线（1px，自绘） */
		constexpr float kBarDivider = 1.0f;

		/* 栏里控件"上下各留 1px"：控件框不跟面板边、也不跟那条分隔线贴在一起
		 * （顶栏 / 底栏共用同一笔账，栏高因此比控件高 2px）。 */
		constexpr float kBarInset = 1.0f;

		/* 底栏（控件高 + 上下各 1px + 上面那条分隔线）的总高：
		 * 实体树按它让出位置，底栏自己也按它定位 —— 一处算，两处用，对得上。 */
		inline float FooterBandHeight()
		{
			return ImGui::GetFrameHeight() + kBarInset * 2.0f + kBarDivider;
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

		/* 过滤时先算出"要显示的实体"：名字与类型都命中的 + 它们的全部祖先 */
		FilterSet visible;
		const FilterSet* filter = nullptr;
		if (HasActiveFilter())
		{
			CollectFilteredEntities(visible);
			filter = &visible;
		}

		const int total_count = (m_pOwnerScene != nullptr)
			? static_cast<int>(m_pOwnerScene->GetRegistry().size()) : 0;
		const int shown_count = (filter != nullptr) ? static_cast<int>(visible.size()) : total_count;

		/* 一个面板 = 顶栏（搜索 + 类型筛选 + 新建）+ 实体树（子窗口，自己滚）+ 底栏（实体计数）。
		 * 面板本身不带上下内边距：顶栏贴面板上边、底栏贴下边（栏高由栏自己说了算）；
		 * 左右内边距保留，顶栏控件与树的每一行对得上同一条竖线。 */
		const ImVec2 pane_padding = ImGui::GetStyle().WindowPadding;
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pane_padding.x, 0.0f));

		ImGui::Begin(Panel::kSceneHierarchy);
		{
			/* 弹层 ID 在面板根作用域上算一次：按钮（开）与菜单（画）的作用域必须一致，
			 * 两边才按同一个 ID 对接。 */
			m_PopupNewEntity = ImGui::GetID("##NewEntityMenu");

			ShowHierarchyTopBar();

			/* ---- 主体：实体树 ----
			 * 高度按绝对几何算：从光标到面板底边、再给底栏让出一档；树装进子窗口（滚动就只在这一段里）。 */
			ImGuiWindow* panel = ImGui::GetCurrentWindow();
			const float panel_bottom = panel->Pos.y + panel->Size.y;
			const ImVec2 panel_min = panel->Pos;
			const ImVec2 panel_max(panel->Pos.x + panel->Size.x, panel_bottom);
			const float body_height = ImMax(
				panel_bottom - ImGui::GetCursorScreenPos().y - FooterBandHeight(),
				ImGui::GetFrameHeight() * 2.0f);

			/* 树里的右键菜单要的是主题的窗口内边距：面板为贴上下边把它压成了 0，
			 * 而弹层的内边距取自开它的那一档样式 —— 不补的话首末条目紧贴弹层背景的上下边
			 * （资源浏览器给它的弹层补的是同一笔账）。子窗口自身的内边距不受影响（走的是零内边距那一支）。 */
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pane_padding);
			ImGui::BeginChild("##HierarchyTree", ImVec2(0.0f, body_height));
			{
				/* 场景名作为根节点，实体树挂在它下面 */
				ShowSceneRootNode(roots, children, pending_delete, filter);

				/* 条目之下的空白区域：拖到这里表示提升到根层级。
				 * 只覆盖空白、不覆盖条目：落在条目上的拖拽目标因为"源与目标相同"会被
				 * ImGui 拒绝（拖起来又原位放下），这时若整窗也算目标，就会误当成提升到根层级。 */
				ImGuiWindow* tree = ImGui::GetCurrentWindow();
				const ImVec2 blank_top = ImGui::GetCursorScreenPos();
				const ImVec2 tree_max(tree->Pos.x + tree->Size.x, tree->Pos.y + tree->Size.y);

				if (tree_max.y > blank_top.y + 1.0f)
				{
					const ImRect blank_area(blank_top, tree_max);
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

				/* 空白处右键，唤出新建（条目来自实体预设注册表） */
				if (ImGui::BeginPopupContextWindow("New", 1, false))
				{
					ShowCreateEntityMenu();
					ImGui::EndPopup();
				}
			}
			ImGui::EndChild();
			ImGui::PopStyleVar();

			/* 拖到面板外松手：挂到场景根，成为一级节点。判据是"鼠标还在不在面板矩形内" ——
			 * 不能用"有没有拖拽目标接收"（拖回原条目上松手时目标会被 ImGui 拒绝，误判成拖出去了）。 */
			const ImVec2 mouse_pos = ImGui::GetMousePos();
			const bool drop_inside_panel = (mouse_pos.x >= panel_min.x && mouse_pos.x < panel_max.x
				&& mouse_pos.y >= panel_min.y && mouse_pos.y < panel_max.y);
			if (!drop_inside_panel)
				ApplyDropOutsidePanel();

			ShowHierarchyFooter(pane_padding, shown_count, total_count);

			/* 顶栏「新建」的下拉菜单画在面板根作用域（按钮也是在这一层开的它），
			 * 并把主题的窗口内边距补回去（面板为贴上下边压成了 0 —— 不补的话
			 * 首末条目紧贴弹层背景的上下边）。 */
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pane_padding);
			DrawNewEntityMenu();
			ImGui::PopStyleVar();
		}
		ImGui::End();

		ImGui::PopStyleVar();

		/* 删除经命令栈落地，可撤销 */
		if (pending_delete)
			DeleteEntity(pending_delete);

		/* 挂接同样延后到这里：遍历中改层级会让本帧的子节点列表失真 */
		ApplyPendingReparent();
	}

	/* 「新建实体」菜单项：条目来自实体预设注册表，新增预设这里零改动。
	 * 分组条目折叠成子菜单（与顶栏下拉共用 BuildEntityMenuEntries 的折叠规则）。 */
	void SceneHierarchy::ShowCreateEntityMenu()
	{
		PROFILE_FUNCTION();

		if (ImGui::BeginMenu("New A Entity"))
		{
			const auto draw_leaf = [this](const EntityTemplateDesc& template_desc)
			{
				if (ImGui::MenuItem(template_desc.Name))
					m_SelectedEntity = CreateEntityFromTemplate(template_desc);
			};

			for (const EntityMenuEntry& entry : BuildEntityMenuEntries(EntityTemplateRegistry::Instance().All()))
			{
				if (entry.Group == nullptr)
				{
					if (entry.Single != nullptr)
						draw_leaf(*entry.Single);
					continue;
				}

				if (ImGui::BeginMenu(entry.Group))
				{
					for (const EntityTemplateDesc* member : entry.Members)
						draw_leaf(*member);
					ImGui::EndMenu();
				}
			}

			ImGui::EndMenu();
		}
	}

	/* 顶栏：左端搜索框，右端「类型筛选 + 新建实体」两枚图标按钮（跟资源浏览器同一套版式：
	 * 放不下就压窄搜索框）。筛选是纯图标按钮，点击弹类型单选菜单，生效时高亮。 */
	void SceneHierarchy::ShowHierarchyTopBar()
	{
		PROFILE_FUNCTION();

		const ImGuiStyle& style = ImGui::GetStyle();
		PanelChrome::HeaderRow row = PanelChrome::BeginHeaderRow(Icons::Id::None);

		/* 控件上下各留 1px：栏高因此比控件高 2px（控件框不贴面板边、也不贴分隔线）。 */
		row.Height += kBarInset * 2.0f;

		const float control_height = ImGui::GetFrameHeight();
		const float control_y = row.Min.y + kBarInset;
		const float gap = style.ItemInnerSpacing.x;

		/* ---- 右端：筛选 + 新建（两枚图标按钮，筛选在左、新建贴最右）----
		 * 新建图标复用资源浏览器的「新建资源」：图案一样，靠 tooltip 和菜单内容区分。 */
		ImGui::SetCursorScreenPos(ImVec2(row.Min.x, control_y));
		if (Icons::IconButton(Icons::Id::NewAsset, ImVec2(control_height, control_height), false,
			"New entity  (empty / 3D / sprite / camera / reflection probe / light)"))
		{
			ImGui::OpenPopup(m_PopupNewEntity);
		}

		/* ---- 左端：搜索框（贴左端）----
		 * 搜索框不撑满：够用就好，宽度多出来的部分留给中间那一段留白；
		 * 空间不足时跟着可用宽度缩，最低 80（再窄就读不了了）。 */
		const float search_preferred = 220.0f;
		const float search_min = 80.0f;

		/* 能用的宽度：从左端那枚按钮之后算起（中间隔一格） */
		const float group_room = row.Right - (row.Min.x + control_height + gap);
		const float search_width = ImMin(search_preferred, ImMax(group_room, search_min));
		const float search_x = row.Right - search_width;

		ImGui::SetCursorScreenPos(ImVec2(search_x, control_y));
		ImGui::SetNextItemWidth(search_width);
		Icons::BeginSearchInput();
		ImGui::InputTextWithHint("##HierarchyFilter", "Search...",
			m_EntityFilter, sizeof(m_EntityFilter));
		Icons::EndSearchInput();

		PanelChrome::EndHeaderRow(row);
	}

	/* 底栏：实体计数贴行右端（上面一条自绘分隔线、文本垂直居中、上下各留 1px，跟顶栏同一
	 * 笔账）。计数含"为挂住命中项而留下的祖先"—— 它就是屏幕上真能数出来的行数。 */
	void SceneHierarchy::ShowHierarchyFooter(const ImVec2& theme_padding, int shown_count, int total_count)
	{
		PROFILE_FUNCTION();

		const ImGuiStyle& style = ImGui::GetStyle();

		char count_text[48] = {};
		if (shown_count != total_count)
			snprintf(count_text, sizeof(count_text), "%d / %d", shown_count, total_count);
		else
			snprintf(count_text, sizeof(count_text), "%d entities", total_count);

		ImGuiWindow* window = ImGui::GetCurrentWindow();

		/* 定位：贴面板的下边（分隔线在栏的正上方）。栏高 = 控件高 + 上下各 1px ——
		 * 比控件高 2px 的那点留白与顶栏同一笔账（文本不跟分隔线、也不跟面板下边贴在一起）。 */
		const float pane_bottom = window->Pos.y + window->Size.y;
		const float footer_top = pane_bottom - ImGui::GetFrameHeight() - kBarInset * 2.0f;
		ImGui::SetCursorScreenPos(ImVec2(window->DC.CursorStartPos.x, footer_top));

		/* 上边线自绘而不是用 ImGui::Separator()：Separator 会吃掉「1px + 行距」，
		 * 于是这一栏就比控件高出一截；自绘的线不参与布局，栏高说得清也量得准。 */
		ImGui::GetWindowDrawList()->AddLine(
			ImVec2(window->DC.CursorStartPos.x, footer_top - kBarDivider * 0.5f),
			ImVec2(window->DC.CursorStartPos.x + ImGui::GetContentRegionAvail().x,
				footer_top - kBarDivider * 0.5f),
			ImGui::GetColorU32(EditorTheme::Token::Border));

		const PanelChrome::HeaderRow row = PanelChrome::BeginHeaderRow(Icons::Id::None);
		const float control_height = ImGui::GetFrameHeight();
		const float control_y = footer_top + kBarInset;
		const float gap = style.ItemInnerSpacing.x;

		/* ---- 左端：类型筛选 ----
		 * 宽度按 BeginCombo 自己的账算（与资源浏览器同一笔）：文字从「控件左端 + FramePadding.x」起、
		 * 到「控件右端 − 箭头区(GetFrameHeight)」为止，前面还要给前置图标留一档 ——
		 * 少算哪一笔，最长的那类名（Reflection Probes）就会被箭头区裁掉一截。 */
		float type_name_width = 0.0f;
		for (int i = 0; i < static_cast<int>(TypeFilter::COUNT); ++i)
		{
			type_name_width = ImMax(type_name_width,
				ImGui::CalcTextSize(TypeFilterName(static_cast<TypeFilter>(i))).x);
		}

		const float arrow_width = control_height;
		const float type_width = type_name_width + arrow_width
			+ style.FramePadding.x + Icons::LeadingIconSpace() + gap;
		const float count_width = ImGui::CalcTextSize(count_text).x;

		/* 退让：放不下时舍类型筛选、计数保留 —— 计数是这一栏的本职；
		 * 筛选在窄面板里还有搜索框兜着（按名字过滤照样能用）。 */
		const float band_span = row.Right - row.Min.x;
		const bool show_type = (type_width + gap + count_width) <= band_span;

		if (show_type)
		{
			const ImVec2 frame_min(row.Min.x, control_y);
			const ImVec2 frame_max(row.Min.x + type_width, control_y + control_height);

			/* 面板的绘制列表：现在就取 —— 弹层打开后"当前窗口"会切到弹层，
			 * 那时 GetWindowDrawList() 拿到的是弹层的列表（自绘的前缀图标会被裁掉），
			 * 下面自己画的东西一律用这一份。 */
			ImDrawList* const row_draw = ImGui::GetWindowDrawList();

			ImGui::SetCursorScreenPos(frame_min);
			ImGui::SetNextItemWidth(type_width);

			/* 预览（图标 + 名字 + 下箭头）自己画，所以给 BeginCombo 传空预览、不要它自带的箭头：
			 * 它的预览文字钉在 FramePadding 上，要给前置图标让位就得把 FramePadding 撑大，
			 * 而弹层的横向内边距正是取自 FramePadding（BeginComboPopup 把
			 * WindowPadding.x 取成当时的 FramePadding.x）—— 撑大它会让弹层条目又挤又偏。
			 * PushStyleVar(WindowPadding) 是给弹层补回上下内边距用的（见函数头注释）。 */
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, theme_padding);
			const bool type_open = ImGui::BeginCombo("##EntityTypeFilter", "", ImGuiComboFlags_NoArrowButton);

			if (type_open)
			{
				for (int i = 0; i < static_cast<int>(TypeFilter::COUNT); ++i)
				{
					const auto filter = static_cast<TypeFilter>(i);
					const bool selected = (filter == m_TypeFilter);

					if (ImGui::Selectable(TypeFilterName(filter), selected))
						m_TypeFilter = filter;

					if (selected)
						ImGui::SetItemDefaultFocus();
				}

				ImGui::EndCombo();
			}

			ImGui::PopStyleVar();

			/* 预览画在弹层关掉之后：这时当前窗口已经回到面板，装饰才落在面板的绘制列表上
			 * （也就压在弹层下面，不会糊到弹层上）。 */
			const float icon_room = style.FramePadding.x + Icons::LeadingIconSpace();
			const float arrow_size = ImGui::GetFontSize() * 0.55f;   /* 与卡片折叠箭头同一档 */
			const ImVec2 arrow_center(frame_max.x - style.FramePadding.x - arrow_size * 0.5f,
				(frame_min.y + frame_max.y) * 0.5f);

			Icons::DrawLeadingIcon(Icons::Id::Filter, frame_min, frame_max);
			PanelChrome::DrawDisclosureArrow(row_draw, arrow_center, arrow_size, true,
				ImGui::GetColorU32(EditorTheme::Token::TextDim));

			/* 名字：与图标同一条基线（行内居中），并裁到箭头区之前 —— 类型名将来变长也不会压到箭头上 */
			row_draw->PushClipRect(ImVec2(frame_min.x + icon_room, frame_min.y),
				ImVec2(arrow_center.x - arrow_size, frame_max.y), true);
			row_draw->AddText(
				ImVec2(frame_min.x + icon_room, (frame_min.y + frame_max.y - ImGui::GetFontSize()) * 0.5f),
				ImGui::GetColorU32(EditorTheme::Token::Text), TypeFilterName(m_TypeFilter));
			row_draw->PopClipRect();

			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Filter by entity type");
		}

		/* ---- 右端：计数贴最右端 ----
		 * 文本自己算居中的 y（Text 把字形框顶放在光标处，"贴着行顶画"会偏高半个内边距）；
		 * 顺带清掉"基线偏移"（同一行前面的控件会把后面的文字整体下移，居中值就被顶掉了）。 */
		const float text_y = control_y + (control_height - ImGui::GetFontSize()) * 0.5f;
		ImGui::SetCursorScreenPos(ImVec2(row.Right - count_width, text_y));
		window->DC.CurrLineTextBaseOffset = 0.0f;
		ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
		ImGui::TextUnformatted(count_text);
		ImGui::PopStyleColor();

		PanelChrome::EndHeaderRow(row, false);
	}

	/* 顶栏「新建」下拉：跟右键菜单同一个数据源（实体预设注册表），是个常驻入口。版式跟
	 * 「添加组件」菜单一样（搜索框 + 列表，打开就清空重聚焦）。没有搜索词时分组收成子菜单；
	 * 有搜索词就平铺列出全部匹配条目，组名也参与匹配。 */
	void SceneHierarchy::DrawNewEntityMenu()
	{
		PROFILE_FUNCTION();

		/* BeginPopupEx 不会自动加 NoTitleBar（BeginPopup 才加）——漏了弹层顶上
		 * 会多出一条空标题栏与折叠钮。 */
		if (!ImGui::BeginPopupEx(m_PopupNewEntity,
				ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
				| ImGuiWindowFlags_NoTitleBar))
			return;

		if (ImGui::IsWindowAppearing())
		{
			m_NewEntityFilter[0] = '\0';
			ImGui::SetKeyboardFocusHere();
		}

		ImGui::SetNextItemWidth(220.0f);
		Icons::BeginSearchInput();
		ImGui::InputTextWithHint("##NewEntitySearch", "Search...", m_NewEntityFilter,
			sizeof(m_NewEntityFilter));
		Icons::EndSearchInput();
		ImGui::Separator();

		const auto draw_leaf = [this](const EntityTemplateDesc& template_desc)
		{
			if (PanelChrome::MenuItemWithIcon(template_desc.Icon, template_desc.Name))
				m_SelectedEntity = CreateEntityFromTemplate(template_desc);
		};

		const std::string needle = ToLowercase(m_NewEntityFilter);

		if (needle.empty())
		{
			for (const EntityMenuEntry& entry : BuildEntityMenuEntries(EntityTemplateRegistry::Instance().All()))
			{
				if (entry.Group == nullptr)
				{
					if (entry.Single != nullptr)
						draw_leaf(*entry.Single);
					continue;
				}

				if (PanelChrome::BeginMenuWithIcon(CategoryIconOf(entry.Group), entry.Group))
				{
					for (const EntityTemplateDesc* member : entry.Members)
						draw_leaf(*member);
					PanelChrome::EndMenuWithIcon();
				}
			}
		}
		else
		{
			bool any_match = false;
			for (const EntityTemplateDesc& template_desc : EntityTemplateRegistry::Instance().All())
			{
				if (!ContainsCaseInsensitive(template_desc.Name, needle)
					&& (template_desc.Category == nullptr
						|| !ContainsCaseInsensitive(template_desc.Category, needle)))
					continue;

				any_match = true;
				draw_leaf(template_desc);
			}

			if (!any_match)
				ImGui::TextDisabled("No matching entity");
		}

		ImGui::EndPopup();
	}

	/* 类型筛选下拉的显示名（与资源浏览器同一套措辞：复数名词、"All types" 放最前） */
	const char* SceneHierarchy::TypeFilterName(TypeFilter filter)
	{
		switch (filter)
		{
		case TypeFilter::All:             return "All types";
		case TypeFilter::Camera:          return "Cameras";
		case TypeFilter::Light:           return "Lights";
		case TypeFilter::Model:           return "Models";
		case TypeFilter::Sprite:          return "Sprites";
		case TypeFilter::ReflectionProbe: return "Reflection Probes";
		case TypeFilter::Other:           return "Other";
		default:                          return "All types";
		}
	}

	/* 实体归到哪一类：直接取它在树里那枚图标所属的类（同一个问题的同一个答案）——
	 * 图标规则将来加了新造型，这里补一条 case 即可；没命中的（通用实体图标）归 Other。 */
	SceneHierarchy::TypeFilter SceneHierarchy::EntityCategory(const Entity& entity)
	{
		switch (ResolveEntityIcon(entity))
		{
		case Icons::Id::Camera:          return TypeFilter::Camera;
		case Icons::Id::LightDirectional:
		case Icons::Id::LightSpot:
		case Icons::Id::LightPoint:      return TypeFilter::Light;
		/* 默认形状是模型实体：类型筛选归 Models（它们持有的就是 Model 组件） */
		case Icons::Id::Cube:
		case Icons::Id::Sphere:
		case Icons::Id::Plane:
		case Icons::Id::Model:           return TypeFilter::Model;
		case Icons::Id::Sprite:          return TypeFilter::Sprite;
		case Icons::Id::ReflectionProbe: return TypeFilter::ReflectionProbe;
		default:                         return TypeFilter::Other;
		}
	}

	/* 过滤词 / 类型筛选命中的实体 + 它们的全部祖先。"命中" = 名字过滤和类型筛选同时通过
	 * （"与"的关系，跟资源浏览器一致）；沿父链往上倒就行。 */
	void SceneHierarchy::CollectFilteredEntities(FilterSet& out) const
	{
		PROFILE_FUNCTION();

		out.clear();
		if (m_pOwnerScene == nullptr || !HasActiveFilter())
			return;

		const std::string needle = ToLowercase(m_EntityFilter);
		const bool has_name = !needle.empty();
		const bool has_type = (m_TypeFilter != TypeFilter::All);

		m_pOwnerScene->GetRegistry().each(
			[&](auto entity_id)
			{
				const Entity entity{ entity_id, m_pOwnerScene };

				if (has_name)
				{
					if (!entity.HasComponent<NameComponent>())
						return;

					if (!ContainsCaseInsensitive(entity.GetComponent<NameComponent>().m_Name.c_str(), needle))
						return;
				}

				if (has_type && EntityCategory(entity) != m_TypeFilter)
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

		/* 固定 str_id：另存为改名的时候，展开状态不该被重置。SpanFullWidth：选中 / 悬停高亮连
		 * 缩进区一起铺满整行（SpanAvailWidth 只铺到文字右侧、左边留一块缺口；跟资源浏览器目录树同一档）。 */
		const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_DefaultOpen;
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

		/* SpanFullWidth：选中 / 悬停高亮撑满整行（连缩进区一起，到行的左右沿）——
		 * SpanAvailWidth 只铺到文字右侧、左边留着缩进的缺口；与资源浏览器目录树同一档。 */
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_DefaultOpen;
		if (m_SelectedEntity == entity)
			flags |= ImGuiTreeNodeFlags_Selected;
		/* 没有子节点就是叶子：不能画成可展开的节点，否则会多出一层空节点 */
		if (!has_children)
			flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

		/* 悬停选中行时 ImGui 画的也是 HeaderHovered（会盖掉选中色）——
		 * 推一档"更亮的选中色"顶住，见 EditorTheme::RowHoverSelected */
		if (m_SelectedEntity == entity)
			ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorTheme::RowHoverSelected);

		/* 空标签：节点自带的名字文字不画，改为下面自绘"图标 + 名字"。
		 * 必须传空字符串：格式化重载会显式传 label_end，而 ImGui 只在 label_end 为 NULL 时
		 * 才隐藏 "##" 之后的内容。 */
		const bool is_opened = ImGui::TreeNodeEx((void*)(uint64_t)(uint32_t)entity, flags, "%s", "");

		if (m_SelectedEntity == entity)
			ImGui::PopStyleColor();

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
			/* 句柄非空 ≠ 实体存在（见 Entity::operator bool 的约定）：选中项可能来自
			 * 撤销后的残留或失效的拾取句柄。组件 GetPtr 对不存在的实体是断言而不是
			 * 返回空指针，所以这里先兜一道。 */
			if (m_SelectedEntity && (m_pOwnerScene == nullptr || !m_pOwnerScene->IsEntityValid(m_SelectedEntity)))
				m_SelectedEntity = {};

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

			/* GetPtr 对没有该组件的实体是断言而不是返回空指针，必须先 Has 再取。 */
			if (desc.Has == nullptr || !desc.Has(m_SelectedEntity))
				break;

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
			ImGuiExt::DrawDragIntUI(field.Name, *reinterpret_cast<int*>(field_ptr), field.DragSpeed);
			break;
		case FieldType::Float:
			ImGuiExt::DrawDragFloatUI(field.Name, *reinterpret_cast<float*>(field_ptr), field.DragSpeed);
			break;
		case FieldType::Vec2:
			ImGuiExt::DrawDragFloat2UI(field.Name, *reinterpret_cast<glm::vec2*>(field_ptr));
			break;
		case FieldType::Vec3:
		{
			auto& vec = *reinterpret_cast<glm::vec3*>(field_ptr);
			/* UniformScale：Scale 类字段 —— 拖任一分量时按住 Shift = 三轴等比 */
			const bool uniform_scale = field.Semantics != nullptr
				&& std::strcmp(field.Semantics, "UniformScale") == 0;
			if (field.Semantics != nullptr && std::strcmp(field.Semantics, "AngleDeg") == 0)
			{
				/* 内部以弧度存储、Inspector 按角度编辑 */
				glm::vec3 degree = glm::degrees(vec);
				ImGuiExt::DrawVec3ControlUI(field.Name, degree, field.ResetValue,
					EditorTheme::Token::PropertyLabelWidth, uniform_scale);
				vec = glm::radians(degree);
			}
			else
			{
				ImGuiExt::DrawVec3ControlUI(field.Name, vec, field.ResetValue,
					EditorTheme::Token::PropertyLabelWidth, uniform_scale);
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

		/* 只有用户真的在操作控件时才生成命令（Gizmo 拖拽这类外部改动不产生历史）。判据要连
		 * "帧开始时活不活跃"一起看：点击类控件在松开的那帧才提交值，那一刻 ActiveId 已经清掉了
		 * —— 只看画完之后的状态，改动会整段丢掉。 */
		if (m_pCommandStack == nullptr || !(item_active || ImGui::IsAnyItemActive()))
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
			{
				ComponentSnapshot before;
				const bool can_record_custom_edit = m_pCommandStack != nullptr
					&& desc.Capture != nullptr && desc.Restore != nullptr;
				if (can_record_custom_edit)
					desc.Capture(entity, before);

				const bool changed = desc.CustomDraw(component);
				if (changed && can_record_custom_edit && before.IsValid())
				{
					ComponentSnapshot after;
					desc.Capture(entity, after);
					if (after.IsValid())
					{
						m_pCommandStack->Execute(CreateUniquePtr<ComponentCustomDrawCommand>(
							entity, desc.Has, desc.Restore, std::move(before), std::move(after), desc.Name));
					}
				}
			}
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
	 * 组件一多，菜单很难找 —— 顶部给一个过滤框，打开即自动聚焦。
	 * 图标是全编辑器「新建 / 添加」入口统一的那枚加号。 */
	void SceneHierarchy::ShowAddComponentButton()
	{
		PROFILE_FUNCTION();

		const float button_size = ImGui::GetFrameHeight();
		if (Icons::IconButton(Icons::Id::NewAsset, ImVec2(button_size, button_size), false, "Add Component"))
			ImGui::OpenPopup("AddComponentPopup");

		if (!ImGui::BeginPopup("AddComponentPopup"))
			return;

		if (ImGui::IsWindowAppearing())
		{
			m_AddComponentFilter[0] = '\0';
			ImGui::SetKeyboardFocusHere();
		}

		ImGui::SetNextItemWidth(220.0f);
		Icons::BeginSearchInput();
		ImGui::InputTextWithHint("##AddComponentSearch", "Search...", m_AddComponentFilter,
			sizeof(m_AddComponentFilter));
		Icons::EndSearchInput();
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

				if (PanelChrome::MenuItemWithIcon(ResolveComponentMenuIcon(desc, variant), label))
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
