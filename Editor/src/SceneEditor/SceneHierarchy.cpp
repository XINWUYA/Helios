#include "Pch.h"
#include "SceneHierarchy.h"
#include "EditorCommon.h"
#include "EditorIcons.h"
#include "PanelChrome.h"
#include "PanelRegistry.h"
#include "Helios/Application/AssetManager.h"
#include "Helios/Common/PathUtils.h"
#include "Helios/Common/Utils.h"
#include "Helios/Scene/SceneCommon.h"
#include "Helios/Scene/ReflectionProbe.h"	/* .probe 详情的烘焙结果预览（BuildPreviewTexture） */
#include "Helios/ImGui/ImGuiExtensions.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include "Helios/VirtualDevice/DeviceShader.h"
#include "EntityTemplateRegistry.h"
#include "Command/ComponentFieldCommand.h"
#include "Command/AddComponentCommand.h"
#include "Command/RemoveComponentCommand.h"
#include "Command/CreateEntityCommand.h"
#include "Command/DeleteEntityCommand.h"
#include "Command/ReparentEntityCommand.h"
#include "Helios/ImGui/EditorTheme.h"
#include "Helios/Scene/Components.h"
#include <glm/gtc/type_ptr.hpp>
#include <tinyxml2.h>
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

			/* 同一实体同一组件的连续改动可合并：拖动材质参数时每帧产生一条快照命令，
			 * 由 CommandStack 的事务窗口（Begin/EndTransaction）圈进来，只保留最初 before 与最新 after。 */
			bool TryMerge(const ICommand& next) override
			{
				const auto* next_command = dynamic_cast<const ComponentCustomDrawCommand*>(&next);
				if (next_command == nullptr)
					return false;
				if (next_command->m_Entity != m_Entity || next_command->m_Restore != m_Restore)
					return false;

				m_After = next_command->m_After;
				return true;
			}

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

		/* 最近一次选择说了算：点实体（层级 / 视口）把属性面板切回组件视图 */
		m_AssetFocus = false;
	}

	/* 资源选中（跨面板能力，由外壳接线：资源浏览器 → 这里）。 */
	void SceneHierarchy::SetAssetSelection(const std::vector<AssetSelectionEntry>& selection)
	{
		m_AssetSelection = selection;

		/* 非空 = 刚点了资源（含同一项被再次点选）：切到资源详情；
		 * 空（切目录 / Esc / 选中项被删）则交回实体视图。 */
		m_AssetFocus = !m_AssetSelection.empty();
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
					SetSelectedEntity(CreateEntityFromTemplate(template_desc));
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
				SetSelectedEntity(CreateEntityFromTemplate(template_desc));
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
			SetSelectedEntity({});

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
			SetSelectedEntity({});
	}

	/* 设置实体可见性（走命令栈、可撤销）：visible 是目标状态 —— 隐藏 = 补上组件置 false、
	 * 显示 = 移除组件（没有组件 = 可见）。相机不受影响；探针隐藏后不烘焙、也不参与 IBL。 */
	void SceneHierarchy::SetEntityVisibility(Entity entity, bool visible)
	{
		PROFILE_FUNCTION();

		if (m_pOwnerScene == nullptr || !m_pOwnerScene->IsEntityValid(entity))
			return;

		/* 无命令栈时也要走同一份应用逻辑：命令自带"补组件 / 去组件"的完整语义 */
		auto command = CreateUniquePtr<SetEntityVisibilityCommand>(m_pOwnerScene, entity, visible);
		if (m_pCommandStack != nullptr)
			m_pCommandStack->Execute(std::move(command));
		else
			command->Do();
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

			if (m_AssetFocus && !m_AssetSelection.empty())
			{
				ShowAssetProperties();
			}
			else if (m_SelectedEntity)
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

		static constexpr const char* kHint = "Select an entity to edit its components, or an asset to inspect it";

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

	/* ==================== 资源详情（属性面板） ====================
	 * 资源浏览器选中资源时显示它的详细内容。最近一次选择说了算：
	 * 点实体即切回组件视图（见 SetSelectedEntity）；这里的绘制全部只读。 */

	namespace
	{
		/* 预览图的尺寸上限：宽度吃满值列，超高 / 超宽的图按比例缩进来 */
		constexpr float kAssetPreviewMaxWidth = 220.0f;
		constexpr float kAssetPreviewMaxHeight = 240.0f;

		/* 资源大类 -> 头部图标（与资源浏览器同一套图标语言；文件夹单独一档） */
		Icons::Id AssetIconOf(const AssetSelectionEntry& entry)
		{
			if (entry.IsFolder)
				return Icons::Id::Directory;

			switch (entry.Kind)
			{
			case AssetFileKind::Image:    return Icons::Id::FileImage;
			case AssetFileKind::Scene:    return Icons::Id::FileScene;
			case AssetFileKind::MtlGraph: return Icons::Id::FileMtlGraph;
			case AssetFileKind::Shader:   return Icons::Id::FileShader;
			case AssetFileKind::Model:    return Icons::Id::FileModel;
			case AssetFileKind::Material: return Icons::Id::FileMaterial;
			case AssetFileKind::Probe:    return Icons::Id::FileProbe;
			case AssetFileKind::Other:    return Icons::Id::File;
			}

			return Icons::Id::File;
		}

		/* 资源大类 -> 显示名（单数；资源浏览器的筛选下拉用的是复数，是另一套词表） */
		const char* AssetKindName(AssetFileKind kind, bool is_folder)
		{
			if (is_folder)
				return "Folder";

			switch (kind)
			{
			case AssetFileKind::Other:    return "File";
			case AssetFileKind::Image:    return "Image";
			case AssetFileKind::Scene:    return "Scene";
			case AssetFileKind::MtlGraph: return "Material Graph";
			case AssetFileKind::Shader:   return "Shader";
			case AssetFileKind::Model:    return "Model";
			case AssetFileKind::Material: return "Material";
			case AssetFileKind::Probe:    return "Reflection Probe";
			}

			return "File";
		}

		/* 烘焙缓存里的一张图 -> 显示名（与探针卡的详情行同口径） */
		const char* BakeImageKindName(ReflectionProbeBakeCache::ImageKind kind)
		{
			switch (kind)
			{
			case ReflectionProbeBakeCache::ImageKind::Environment: return "Environment";
			case ReflectionProbeBakeCache::ImageKind::Irradiance:  return "Irradiance";
			case ReflectionProbeBakeCache::ImageKind::Prefilter:   return "Prefilter";
			}

			return "Image";
		}

		/* 可选 Shader：Assets 下所有 .glsl 源（相对路径，字典序）。
		 * 跳过 Cache（编译产物目录，里面是 .metal）；.glsl 允许建在任何子目录，所以整树扫 ——
		 * 与材质编辑缓冲共用同一缓存键，只在选中材质或文件变化时重扫一次。 */
		std::vector<std::string> CollectShaderOptions()
		{
			std::vector<std::string> options;

			std::error_code error;
			for (std::filesystem::recursive_directory_iterator iter(g_AssetsPath,
					std::filesystem::directory_options::skip_permission_denied, error), end;
				iter != end; iter.increment(error))
			{
				if (error)
					break;

				const std::filesystem::directory_entry& item = *iter;
				if (item.is_directory())
				{
					if (item.path().filename() == "Cache")
						iter.disable_recursion_pending();
					continue;
				}

				if (!item.is_regular_file())
					continue;

				std::string extension = PathToUtf8(item.path().extension());
				for (char& character : extension)
				{
					if (character >= 'A' && character <= 'Z')
						character = static_cast<char>(character - 'A' + 'a');
				}
				if (extension != ".glsl")
					continue;

				options.emplace_back(RELATIVE_PATH(PathToUtf8(item.path())));
			}

			std::sort(options.begin(), options.end());
			return options;
		}
	}

	void SceneHierarchy::ShowAssetProperties()
	{
		PROFILE_FUNCTION();

		/* 头部：资源图标 + 名字（只读 —— 改名走资源浏览器的右键菜单，资源名不是实体名） */
		const AssetSelectionEntry& first = m_AssetSelection.front();
		const PanelChrome::HeaderRow header = PanelChrome::BeginHeaderRow(AssetIconOf(first));
		PanelChrome::DrawHeaderTitle(header, first.Name, header.Right - header.TitleX);
		PanelChrome::EndHeaderRow(header);

		if (m_AssetSelection.size() == 1)
			DrawAssetDetailsCard(first);
		else
			DrawMultiAssetCard();
	}

	void SceneHierarchy::DrawAssetDetailsCard(const AssetSelectionEntry& entry)
	{
		PROFILE_FUNCTION();

		const std::filesystem::path absolute_path = g_AssetsPath / PathFromUtf8(entry.Path);
		const PanelChrome::Card card = PanelChrome::BeginCard(
			AssetKindName(entry.Kind, entry.IsFolder), AssetIconOf(entry));

		ImGuiExt::DrawCommonTextUI("Path", entry.Path);
		if (entry.IsFolder)
			ImGuiExt::DrawCommonTextUI("Items", std::to_string(entry.ChildCount));
		else
			ImGuiExt::DrawCommonTextUI("Size", FormatFileSize(entry.SizeBytes));
		ImGuiExt::DrawCommonTextUI("Modified", FormatFileWriteTime(absolute_path));

		/* 类型相关的深挖行（图片尺寸 / 场景统计）：按 (路径, 写入时间) 缓存 */
		RefreshAssetDetailRows(entry);
		for (const auto& [label, value] : m_AssetDetailRows)
			ImGuiExt::DrawCommonTextUI(label, value);

		/* 图片给一张更大的预览：与资源浏览器的缩略图共用 TextureAssetManager 的缓存
		 * （同一张贴图不会加载两份；headless 下没有图形上下文时贴图为空，跳过即可）。 */
		if (!entry.IsFolder && entry.Kind == AssetFileKind::Image)
		{
			const SharedPtr<DeviceTexture> preview = TextureAssetManager::Instance().GetOrCreateTexture(
				PathToUtf8(absolute_path));
			if (preview != nullptr && preview->IsLoaded())
			{
				ImGui::Spacing();

				const float max_width = ImMin(ImGui::GetContentRegionAvail().x, kAssetPreviewMaxWidth);
				const float aspect = static_cast<float>(preview->GetHeight())
					/ static_cast<float>(ImMax(preview->GetWidth(), 1u));

				float width = max_width;
				float height = width * aspect;
				if (height > kAssetPreviewMaxHeight)
				{
					height = kAssetPreviewMaxHeight;
					width = height / ImMax(aspect, 1.0e-6f);
				}

				ImGui::Image((ImTextureID)preview.get(), ImVec2(width, height),
					ImVec2(0, 1), ImVec2(1, 0));
			}
		}

		PanelChrome::EndCard(card);

		/* .probe：三张烘焙图各一张卡片 —— 同级平铺在详情卡之后（卡片通道不可嵌套，
		 * "卡中卡"画不出来，与材质卡排在组件卡之后是同一条路数）。
		 * 图是十字展开预览；无图形上下文（headless）时只有规格行，图跳过。 */
		for (size_t index = 0; index < m_AssetProbeCards.size(); ++index)
		{
			ProbeImageCard& probe_image = m_AssetProbeCards[index];

			/* 卡片键按索引：三张各一卡、折叠状态互不影响 */
			ImGui::PushID(static_cast<int>(index));

			const PanelChrome::Card image_card = PanelChrome::BeginCard(
				probe_image.Label.c_str(), Icons::Id::FileProbe);

			if (image_card.Open)
			{
				ImGuiExt::DrawCommonTextUI("Texture", probe_image.Spec);

				/* 多层 mip 才有的切换行：切哪层就看哪层的十字展开 —— 高层看整体结构、
				 * 低层（模糊）看细节（预滤波的高层就是更粗糙的那档）。
				 * 选项 = 层号 + 该层面边长；初始选中 = 自动挑的那层。 */
				if (probe_image.MipCount > 1)
				{
					std::vector<std::string> mip_options;
					mip_options.reserve(static_cast<size_t>(probe_image.MipCount));
					for (int level = 0; level < probe_image.MipCount; ++level)
					{
						mip_options.push_back(std::to_string(level) + " ("
							+ std::to_string(ReflectionProbe::PreviewMipFaceSize(
								probe_image.Mip0Size, static_cast<uint32_t>(level))) + ")");
					}

					int mip = probe_image.Mip;
					bool mip_changed = false;
					ImGuiExt::DrawComboUI("Mip", mip_options, mip,
						[&mip_changed](int) { mip_changed = true; });
					if (mip_changed)
						ApplyProbeCardMip(index, mip);
				}

				if (probe_image.Texture != nullptr)
				{
					ImGui::Spacing();

					/* 4:3 的十字图；宽度跟"图片预览"同一档上限，居中 */
					const float available = ImGui::GetContentRegionAvail().x;
					const float image_width = ImMin(available, kAssetPreviewMaxWidth);
					const float image_height = image_width * 0.75f;

					ImGui::SetCursorPosX(ImGui::GetCursorPosX()
						+ ImMax((available - image_width) * 0.5f, 0.0f));

					/* UV 不翻转：预览图第 0 行（+Y 面）就在顶部 —— 与图片贴图的
					 * (0,1)-(1,0) 相反，那些贴图在上传前被垂直翻转（IsFlipV） */
					ImGui::Image((ImTextureID)probe_image.Texture.get(),
						ImVec2(image_width, image_height), ImVec2(0, 0), ImVec2(1, 1));

					if (ImGui::IsItemHovered())
					{
						ImGui::BeginTooltip();
						ImGui::Image((ImTextureID)probe_image.Texture.get(),
							ImVec2(320.0f, 240.0f), ImVec2(0, 0), ImVec2(1, 1));
						ImGui::TextUnformatted(probe_image.Label.c_str());
						ImGui::EndTooltip();
					}
				}
			}

			PanelChrome::EndCard(image_card);
			ImGui::PopID();
		}

		/* .mtl：材质编辑卡（同 probe 图卡：同级平铺在详情卡之后）—— Shader 可选可改、
		 * 参数可就地编辑，「Apply」把编辑缓冲序列化回文件。 */
		DrawAssetMaterialCards(absolute_path);
	}

	void SceneHierarchy::DrawMultiAssetCard()
	{
		PROFILE_FUNCTION();

		const PanelChrome::Card card = PanelChrome::BeginCard("Assets", Icons::Id::File);

		int kind_counts[8] = {};
		int folder_count = 0;
		uintmax_t total_bytes = 0;
		for (const AssetSelectionEntry& entry : m_AssetSelection)
		{
			total_bytes += entry.SizeBytes;
			if (entry.IsFolder)
				++folder_count;
			else
				++kind_counts[static_cast<size_t>(entry.Kind)];
		}

		ImGuiExt::DrawCommonTextUI("Items", std::to_string(m_AssetSelection.size()));
		ImGuiExt::DrawCommonTextUI("Total Size", FormatFileSize(total_bytes));

		/* 类型分布："2 Images, 1 Scene, 1 Folder"（按大类的枚举顺序，文件夹收尾） */
		std::string types;
		const auto append = [&types](int count, const char* name)
		{
			if (count <= 0)
				return;

			if (!types.empty())
				types += ", ";
			types += std::to_string(count) + " " + name + ((count > 1) ? "s" : "");
		};

		for (size_t kind = 0; kind < 8; ++kind)
			append(kind_counts[kind], AssetKindName(static_cast<AssetFileKind>(kind), false));
		append(folder_count, "Folder");

		ImGuiExt::DrawCommonTextUI("Types", types);

		PanelChrome::EndCard(card);
	}

	void SceneHierarchy::RefreshAssetDetailRows(const AssetSelectionEntry& entry)
	{
		PROFILE_FUNCTION();

		/* 深挖详情要打开文件（图片读文件头 / 场景解析 XML），不必每帧做：
		 * 只在选中项或它的写入时间变化时重算。 */
		const std::filesystem::path absolute_path = g_AssetsPath / PathFromUtf8(entry.Path);

		std::error_code error;
		const std::filesystem::file_time_type write_time =
			std::filesystem::last_write_time(absolute_path, error);
		if (error)
		{
			m_AssetDetailPath.clear();
			m_AssetDetailRows.clear();
			m_AssetProbeCards.clear();
			m_AssetMaterialGroup = nullptr;
			m_AssetMaterialShaderOptions.clear();
			m_AssetMaterialDirty = false;
			return;
		}

		if (entry.Path == m_AssetDetailPath && write_time == m_AssetDetailWriteTime)
			return;

		m_AssetDetailPath = entry.Path;
		m_AssetDetailWriteTime = write_time;
		m_AssetDetailRows.clear();
		m_AssetProbeCards.clear();
		m_AssetMaterialGroup = nullptr;
		m_AssetMaterialShaderOptions.clear();
		m_AssetMaterialDirty = false;

		if (entry.Kind == AssetFileKind::Image)
		{
			int width = 0;
			int height = 0;
			int channels = 0;
			if (QueryImageInfo(PathToUtf8(absolute_path), width, height, channels))
			{
				m_AssetDetailRows.emplace_back("Dimensions",
					std::to_string(width) + " x " + std::to_string(height));
				m_AssetDetailRows.emplace_back("Channels", std::to_string(channels));
			}
		}
		else if (entry.Kind == AssetFileKind::Scene)
		{
			/* 场景统计：实体数 + 各类别的数量（本地解析一遍，结果缓存在成员里） */
			auto scene_file = std::unique_ptr<FILE, decltype(&std::fclose)>(
				OpenUtf8File(absolute_path, "rb"), &std::fclose);
			if (scene_file == nullptr)
				return;

			tinyxml2::XMLDocument doc;
			if (doc.LoadFile(scene_file.get()) != tinyxml2::XML_SUCCESS)
				return;

			const tinyxml2::XMLElement* scene_root = doc.FirstChildElement("Scene");
			const tinyxml2::XMLElement* entities_root = (scene_root != nullptr)
				? scene_root->FirstChildElement("Entities") : nullptr;
			if (entities_root == nullptr)
				return;

			int entity_count = 0;
			int light_count = 0;
			int camera_count = 0;
			int probe_count = 0;
			for (const tinyxml2::XMLElement* entity = entities_root->FirstChildElement("Entity");
				entity != nullptr; entity = entity->NextSiblingElement("Entity"))
			{
				++entity_count;
				if (entity->FirstChildElement("Light") != nullptr)
					++light_count;
				if (entity->FirstChildElement("Camera") != nullptr)
					++camera_count;
				if (entity->FirstChildElement("ReflectionProbe") != nullptr)
					++probe_count;
			}

			m_AssetDetailRows.emplace_back("Entities", std::to_string(entity_count));
			if (light_count > 0)
				m_AssetDetailRows.emplace_back("Lights", std::to_string(light_count));
			if (camera_count > 0)
				m_AssetDetailRows.emplace_back("Cameras", std::to_string(camera_count));
			if (probe_count > 0)
				m_AssetDetailRows.emplace_back("Probes", std::to_string(probe_count));
		}
		else if (entry.Kind == AssetFileKind::Probe)
		{
			/* 反射探针的烘焙缓存：每张立方图（环境 / 辐照度 / 预滤波）攒一张图卡
			 * （名字 + 规格 + 十字展开预览）—— 由调用方在详情卡之后同级平铺画出。
			 * 预览要上传纹理（无图形上下文时为空），规格行是纯 CPU 的，照常显示。 */
			ReflectionProbeBakeCache::Data data;
			if (!ReflectionProbeBakeCache::Read(PathToUtf8(absolute_path), data))
				return;	/* 损坏 / 版本不符：图卡留空（通用行仍显示大小与时间） */

			for (const ReflectionProbeBakeCache::Image& image : data.Images)
			{
				ProbeImageCard image_card;
				image_card.Label = BakeImageKindName(image.Kind);

				const char* format_name = GetEnumName(image.Format);
				image_card.Spec = std::to_string(image.Size) + " x " + std::to_string(image.Size)
					+ ", " + (format_name != nullptr ? format_name : "?")
					+ ", " + std::to_string(image.Mips.size())
					+ (image.Mips.size() > 1 ? " mips" : " mip");

				image_card.SourcePath = PathToUtf8(absolute_path);
				image_card.TextureName = PathToUtf8(absolute_path.filename()) + "_" + image_card.Label;
				image_card.ImageIndex = static_cast<int>(m_AssetProbeCards.size());
				image_card.MipCount = static_cast<int>(std::max<size_t>(image.Mips.size(), 1));
				image_card.Mip0Size = image.Size;
				/* 初始显示"自动挑的那层"（封面到 ~64px）—— 与探针卡里的预览同口径 */
				image_card.Mip = static_cast<int>(ReflectionProbe::AutoPreviewMip(image));
				image_card.Texture = ReflectionProbe::BuildPreviewTexture(
					image_card.TextureName, image, image_card.Mip);

				m_AssetProbeCards.push_back(std::move(image_card));
			}
		}
		else if (entry.Kind == AssetFileKind::Material)
		{
			/* 材质资产：整个文件读进编辑缓冲（全部条目，引用形态条目只显示不编辑）。
			 * 条目标题下的「Materials」行给出条目数；读不回来的文件明确说 unreadable。 */
			auto group = CreateSharedPtr<MaterialGroup>();
			if (group->Deserializer(PathToUtf8(absolute_path)))
			{
				m_AssetMaterialGroup = std::move(group);
				m_AssetDetailRows.emplace_back("Materials",
					std::to_string(m_AssetMaterialGroup->GetEntries().size()));
			}
			else
			{
				m_AssetDetailRows.emplace_back("Materials", "unreadable");
			}

			m_AssetMaterialShaderOptions = CollectShaderOptions();
			m_AssetMaterialDirty = false;	/* 刚读上来的缓冲与文件一致（Apply 的禁用依据） */
		}
	}

	/* 切换某张烘焙图卡的预览 mip：重开缓存文件（数据不常驻 —— 切层是低频动作，
	 * 重读一遍文件最省内存）、按新层级重生成十字展开图。
	 * 读不到（损坏 / 丢失 / 序号越界）：图留空，卡照常显示。 */
	void SceneHierarchy::ApplyProbeCardMip(size_t card_index, int mip)
	{
		ProbeImageCard& card = m_AssetProbeCards[card_index];
		card.Mip = mip;

		ReflectionProbeBakeCache::Data data;
		if (card.SourcePath.empty() || !ReflectionProbeBakeCache::Read(card.SourcePath, data)
			|| card.ImageIndex < 0 || static_cast<size_t>(card.ImageIndex) >= data.Images.size())
		{
			card.Texture = nullptr;
			return;
		}

		card.Texture = ReflectionProbe::BuildPreviewTexture(
			card.TextureName, data.Images[card.ImageIndex], card.Mip);
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

	/* ==================== 材质卡（Model 组件的附属，每槽一张）====================
	 * 卡片背景走绘制通道、不能嵌套 —— 材质详情就平铺在 Model 卡后面。编辑语义跟组件卡一致：
	 * 改参数自动实例化（改动那一刻才克隆写回），撤销走"组件快照 + 事务合并"。 */

	namespace
	{
		/* 材质没有 Shader 时按钮上的占位文案（菜单照常能开，选一个即可补上） */
		constexpr const char* kNoShaderOption = "(no shader)";

		/* 反射参数 → 显示行：材质还没有这条时用它撑起一行 ——
		 * 默认值取 Shader 源码声明的（如 u_AlbedoTilingOffset = vec4(1,1,0,0)），
		 * 没有就按类型给零值；贴图没有"默认"，显示成空槽（拖入才落材质）。 */
		MaterialParamInfo MakeDefaultMaterialParamRow(const ReflectedMaterialParam& reflected)
		{
			const std::any& declared = reflected.Default;
			switch (reflected.Type)
			{
			case ParamType::Texture:
				return Material::MakeTextureParam(reflected.Name, nullptr);
			case ParamType::Int:
				return { ParamType::Int, reflected.Name, declared.has_value() ? declared : std::any(0) };
			case ParamType::Float:
				return { ParamType::Float, reflected.Name, declared.has_value() ? declared : std::any(0.0f) };
			case ParamType::Vec2:
				return { ParamType::Vec2, reflected.Name, declared.has_value() ? declared : std::any(glm::vec2(0.0f)) };
			case ParamType::Vec3:
				return { ParamType::Vec3, reflected.Name, declared.has_value() ? declared : std::any(glm::vec3(0.0f)) };
			case ParamType::Vec4:
				return { ParamType::Vec4, reflected.Name, declared.has_value() ? declared : std::any(glm::vec4(0.0f)) };
			case ParamType::Mat4:
				return { ParamType::Mat4, reflected.Name, std::any(glm::mat4(1.0f)) };
			}

			return { reflected.Type, reflected.Name, std::any{} };
		}

		/* 材质参数行的显示清单：Shader 反射是参数集合的唯一来源（反射声明什么就列什么，跟材质
		 * 里残留的旧参数无关）；值优先取材质里已有的，没有的就显示反射默认值。反射不到就返回空、
		 * 不回退旧参数表（让调用方画报错），免得旧参数被显示成"默认材质"误导人。 */
		std::vector<MaterialParamInfo> BuildMaterialParamRows(const Material& material)
		{
			std::vector<MaterialParamInfo> rows;

			const SharedPtr<DeviceShader> shader = material.GetShader();
			if (shader == nullptr)
				return rows;

			const ShaderReflectionData& reflection = shader->GetReflectionData();
			const auto& parameters = material.GetAllParameters();
			rows.reserve(reflection.MaterialParams.size());
			for (const ReflectedMaterialParam& reflected : reflection.MaterialParams)
			{
				const auto iter = parameters.find(ToID(reflected.Name));
				rows.push_back(iter != parameters.end() ? iter->second : MakeDefaultMaterialParamRow(reflected));
			}

			std::sort(rows.begin(), rows.end(),
				[](const MaterialParamInfo& lhs, const MaterialParamInfo& rhs) { return lhs.Name < rhs.Name; });
			return rows;
		}

		/* 参数无法按 Shader 反射时的报错提示行（Danger 色）：两种状态分开说清 ——
		 * 没 Shader / 有 Shader 但反射为空（后端不支持或声明识别不出）。 */
		void DrawMaterialReflectionNotice(const Material& material)
		{
			const char* message = (material.GetShader() == nullptr)
				? "No shader loaded — cannot reflect parameters"
				: "Shader reflection unavailable — parameters not listed";

			ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::Danger);
			ImGui::TextWrapped("%s", message);
			ImGui::PopStyleColor();
		}

		/* Shader 菜单树：按目录分层的节点（文件夹 = Children 非空；文件 = Path 为完整相对路径） */
		struct ShaderMenuNode
		{
			std::string Name;                      /* 显示名（文件 / 文件夹名） */
			std::string Path;                      /* 文件 = 完整相对路径；文件夹 = 空 */
			std::vector<ShaderMenuNode> Children;  /* 文件夹的子项 */

			[[nodiscard]] bool IsFolder() const { return Path.empty(); }
		};

		/* 把"可选 Shader 的相对路径清单"装成一棵按目录分层的菜单树（同名文件夹归并）。
		 * 清单本身是字典序：同一层级里文件夹与文件各自保持排好的顺序 ——
		 * 画的时候文件夹先、文件后（见 DrawShaderMenuLevel）。 */
		std::vector<ShaderMenuNode> BuildShaderMenuTree(const std::vector<std::string>& options)
		{
			std::vector<ShaderMenuNode> roots;

			for (const std::string& option : options)
			{
				std::vector<ShaderMenuNode>* level = &roots;
				size_t begin = 0;

				while (true)
				{
					const size_t slash = option.find('/', begin);
					if (slash == std::string::npos)
						break;

					const std::string folder = option.substr(begin, slash - begin);
					auto found = std::find_if(level->begin(), level->end(),
						[&folder](const ShaderMenuNode& node)
						{
							return node.IsFolder() && node.Name == folder;
						});

					if (found == level->end())
					{
						level->push_back(ShaderMenuNode{ folder, {}, {} });
						found = level->end() - 1;
					}

					level = &found->Children;
					begin = slash + 1;
				}

				level->push_back(ShaderMenuNode{ option.substr(begin), option, {} });
			}

			return roots;
		}

		/* 画一层 Shader 菜单：文件夹先（悬停展开子菜单）、文件后（点一条 = 选中）。
		 * 当前 Shader 打一枚对勾（Accent，行右缘）；选中后把整条菜单链收起。 */
		void DrawShaderMenuLevel(const std::vector<ShaderMenuNode>& nodes, const std::string& current,
			std::string& out_picked)
		{
			for (const ShaderMenuNode& node : nodes)
			{
				if (!node.IsFolder())
					continue;

				if (PanelChrome::BeginMenuWithIcon(Icons::Id::Directory, node.Name.c_str()))
				{
					DrawShaderMenuLevel(node.Children, current, out_picked);
					PanelChrome::EndMenuWithIcon();
				}
			}

			for (const ShaderMenuNode& node : nodes)
			{
				if (node.IsFolder())
					continue;

				/* 对勾行的默认行为是"点了不收起"（连点开关用）；单选的场合点完自己收起 ——
				 * CloseCurrentPopup 会顺着 ChildMenu 链把整个菜单一起带走。 */
				bool checked = (node.Path == current);
				if (PanelChrome::MenuItemToggleWithIcon(Icons::Id::FileShader, node.Name.c_str(), &checked))
				{
					out_picked = node.Path;
					ImGui::CloseCurrentPopup();
				}
			}
		}

		/* 把槽的绑定换成"实例"（若已是实例则原样返回）。
		 * 返回值：可编辑的实例材质；component_changed 标记组件数据被修改（撤销靠它）。 */
		SharedPtr<Material> EnsureSlotInstance(ModelComponent& component, const Model& model,
			int slot_index, bool& component_changed)
		{
			const MaterialSlot* slot = model.GetSlotByIndex(slot_index);
			if (slot == nullptr)
				return nullptr;

			auto iter = component.m_SlotOverrides.find(slot->Name);
			if (iter != component.m_SlotOverrides.end() && iter->second.IsInstance && iter->second.pMaterial != nullptr)
				return iter->second.pMaterial;

			/* 从"当前生效材质"（覆盖 ?: 默认 ?: 白模）克隆 */
			SharedPtr<Material> source;
			std::string source_path;
			if (iter != component.m_SlotOverrides.end() && iter->second.pMaterial != nullptr)
			{
				source = iter->second.pMaterial;
				source_path = iter->second.pMaterial->GetPath();
			}
			else
			{
				source = (slot->pDefault != nullptr) ? slot->pDefault : Material::BuiltinWhite();
				source_path = source->GetPath();
			}

			MaterialSlotOverride slot_override;
			slot_override.pMaterial = source->Clone();
			slot_override.IsInstance = true;
			slot_override.SourceAssetPath = std::move(source_path);
			component.m_SlotOverrides[slot->Name] = std::move(slot_override);
			component_changed = true;

			return component.m_SlotOverrides[slot->Name].pMaterial;
		}

		/* 参数改动写回：确保实例 → SetParameters 到实例（Texture 走 SetTexture 的绑定解析） */
		void WriteBackMaterialParam(ModelComponent& component, const Model& model, int slot_index,
			ParamType type, const std::string& name, const std::any& value, bool& component_changed)
		{
			SharedPtr<Material> instance = EnsureSlotInstance(component, model, slot_index, component_changed);
			if (instance == nullptr)
				return;

			if (type == ParamType::Texture)
				instance->SetTexture(name, std::any_cast<SharedPtr<DeviceTexture>>(value));
			else
				instance->SetParameters(type, name, value);
		}

		/* 光栅化状态改动写回：确保实例 → SetRasterState 到实例（与参数同一套撤销路径） */
		void WriteBackMaterialRasterState(ModelComponent& component, const Model& model, int slot_index,
			const RenderRasterState& state, bool& component_changed)
		{
			SharedPtr<Material> instance = EnsureSlotInstance(component, model, slot_index, component_changed);
			if (instance == nullptr)
				return;

			instance->SetRasterState(state);
		}

		/* 一条材质参数行：值画在临时缓冲上，改动经 write_back 落进目标材质（槽位 → 自动实例化
		 * 写回；资产编辑缓冲 → 直接写）。行对齐：标签跟 frame 高控件垂直居中。贴图行只画缩略图
		 * （点击定位、拖入替换、悬停看大图）。 */
		void DrawMaterialParamRow(const MaterialParamInfo& param_info,
			const SceneHierarchy::AssetRevealFunc& reveal_asset,
			const std::function<void(ParamType, const std::string&, const std::any&)>& write_back)
		{
			bool edited = false;
			const std::string& param_name = param_info.Name;
			const float kLabelWidth = EditorTheme::Token::PropertyLabelWidth;

			switch (param_info.Type)
			{
			case ParamType::Texture:
				{
					auto texture_info = std::any_cast<std::pair<SharedPtr<DeviceTexture>, uint32_t>>(param_info.Value);
					auto texture = texture_info.first; /* 临时副本 */

					ImGuiExt::BeginPropertyRow(param_name.c_str(), kLabelWidth, true);

					const auto& show_texture = texture ? texture
						: TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH("Textures/default.png"));

					/* 缩略图与行高同高（frame 高含 1px 外框）：逐行对齐、不撑行 */
					const float thumbnail = ImGui::GetFrameHeight() - 2.0f;
					const bool clicked = ImGui::ImageButton((ImTextureID)show_texture.get(), ImVec2(thumbnail, thumbnail),
						ImVec2(0, 1), ImVec2(1, 0), 1, ImVec4(0, 0, 0, 0), ImVec4(1, 1, 1, 1));

					if (ImGui::BeginDragDropTarget())
					{
						if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RESOURCE_BROWSER_ITEM"))
						{
							std::filesystem::path relative_path;
							if (payload->DataSize > 1 && TryPathFromUtf8Payload(
								payload->Data, static_cast<size_t>(payload->DataSize), relative_path))
							{
								const auto new_texture = TextureAssetManager::Instance().GetOrCreateTexture(
									PathToUtf8(g_AssetsPath / relative_path));
								if (new_texture->IsLoaded())
								{
									texture = new_texture;
									edited = true;
								}
							}
						}
						ImGui::EndDragDropTarget();
					}

					if (ImGui::IsItemHovered())
					{
						ImGui::BeginTooltip();
						ImGui::Image((ImTextureID)show_texture.get(), ImVec2(200, 200), ImVec2(0, 1), ImVec2(1, 0));
						ImGui::TextDisabled("Click to reveal in Assets");
						ImGui::EndTooltip();
					}

					/* 点击缩略图 = 在资源浏览器里定位这张贴图 */
					if (clicked && reveal_asset && show_texture != nullptr)
					{
						const std::string relative = RELATIVE_PATH(show_texture->GetPath());
						if (!relative.empty() && relative.rfind("..", 0) != 0)
							reveal_asset(relative);
					}

					ImGuiExt::EndPropertyRow();

					if (edited)
						write_back(ParamType::Texture, param_name, texture);
				}
				break;
			case ParamType::Int:
				{
					int value = std::any_cast<int>(param_info.Value);
					const float value_width = ImGuiExt::BeginPropertyRow(param_name.c_str(), kLabelWidth, true);
					ImGui::SetNextItemWidth(value_width);
					edited = ImGui::DragInt("##Value", &value, 0.1f);
					ImGuiExt::EndPropertyRow();
					if (edited)
						write_back(ParamType::Int, param_name, value);
				}
				break;
			case ParamType::Float:
				{
					float value = std::any_cast<float>(param_info.Value);
					const float value_width = ImGuiExt::BeginPropertyRow(param_name.c_str(), kLabelWidth, true);
					ImGui::SetNextItemWidth(value_width);
					edited = ImGui::DragFloat("##Value", &value, 0.01f);
					ImGuiExt::EndPropertyRow();
					if (edited)
						write_back(ParamType::Float, param_name, value);
				}
				break;
			case ParamType::Vec2:
				{
					glm::vec2 value = std::any_cast<glm::vec2>(param_info.Value);
					const float value_width = ImGuiExt::BeginPropertyRow(param_name.c_str(), kLabelWidth, true);
					ImGui::SetNextItemWidth(value_width);
					edited = ImGui::DragFloat2("##Value", glm::value_ptr(value), 0.01f);
					ImGuiExt::EndPropertyRow();
					if (edited)
						write_back(ParamType::Vec2, param_name, value);
				}
				break;
			case ParamType::Vec3:
				{
					glm::vec3 value = std::any_cast<glm::vec3>(param_info.Value);
					const float value_width = ImGuiExt::BeginPropertyRow(param_name.c_str(), kLabelWidth, true);
					ImGui::SetNextItemWidth(value_width);
					edited = ImGui::DragFloat3("##Value", glm::value_ptr(value), 0.01f);
					ImGuiExt::EndPropertyRow();
					if (edited)
						write_back(ParamType::Vec3, param_name, value);
				}
				break;
			case ParamType::Vec4:
				{
					glm::vec4 value = std::any_cast<glm::vec4>(param_info.Value);
					const float value_width = ImGuiExt::BeginPropertyRow(param_name.c_str(), kLabelWidth, true);
					ImGui::SetNextItemWidth(value_width);
					edited = ImGui::DragFloat4("##Value", glm::value_ptr(value), 0.01f);
					ImGuiExt::EndPropertyRow();
					if (edited)
						write_back(ParamType::Vec4, param_name, value);
				}
				break;
			default:
				/* Mat4 等没做编辑器，跳过（只读展示也省） */
				break;
			}
		}

		/* ---- 光栅化状态（RenderRasterState）的行 ---- */

		/* 各枚举的显示名（顺序与 RenderCommon.h 里的枚举定义一一对应） */
		const std::vector<std::string>& CullModeOptions()
		{
			static const std::vector<std::string> options = { "None", "Front", "Back", "Front And Back" };
			return options;
		}

		const std::vector<std::string>& FrontFaceOptions()
		{
			static const std::vector<std::string> options = { "CW", "CCW" };
			return options;
		}

		const std::vector<std::string>& BlendEquationOptions()
		{
			static const std::vector<std::string> options = { "Add", "Subtract", "Reverse Subtract", "Min", "Max" };
			return options;
		}

		const std::vector<std::string>& BlendFuncOptions()
		{
			static const std::vector<std::string> options = {
				"Zero", "One", "Src Color", "One Minus Src Color", "Dst Color", "One Minus Dst Color",
				"Src Alpha", "One Minus Src Alpha", "Dst Alpha", "One Minus Dst Alpha", "Src Alpha Saturate" };
			return options;
		}

		const std::vector<std::string>& CompareFuncOptions()
		{
			static const std::vector<std::string> options = {
				"Less Equal", "Greater Equal", "Less", "Greater", "Equal", "Not Equal", "Always", "Never" };
			return options;
		}

		/* 光栅化状态的行：直接画在传入的 state 上；返回值 = 是否有改动
		 * （调用方决定记脏 / 实例化写回 / 热更新 —— 两种材质卡共用同一套行）。 */
		bool DrawMaterialRasterStateRows(RenderRasterState& state)
		{
			bool changed = false;

			int cull_mode = static_cast<int>(state.CullMode);
			ImGuiExt::DrawComboUI("Cull Mode", CullModeOptions(), cull_mode,
				[&](int picked) { state.CullMode = static_cast<CullMode>(picked); changed = true; });

			int front_face = static_cast<int>(state.FrontFaceType);
			ImGuiExt::DrawComboUI("Front Face", FrontFaceOptions(), front_face,
				[&](int picked) { state.FrontFaceType = static_cast<FrontFaceType>(picked); changed = true; });

			/* 位域字段不能绑 bool&：勾选框画在临时量上、改动时写回 */
			bool enable_blend = state.EnableBlend;
			if (ImGuiExt::DrawCheckboxUI("Blend", enable_blend))
			{
				state.EnableBlend = enable_blend;
				changed = true;
			}

			int blend_equation_rgb = static_cast<int>(state.BlendEquationRGB);
			ImGuiExt::DrawComboUI("Blend Op RGB", BlendEquationOptions(), blend_equation_rgb,
				[&](int picked) { state.BlendEquationRGB = static_cast<BlendEquation>(picked); changed = true; });

			int blend_equation_a = static_cast<int>(state.BlendEquationA);
			ImGuiExt::DrawComboUI("Blend Op A", BlendEquationOptions(), blend_equation_a,
				[&](int picked) { state.BlendEquationA = static_cast<BlendEquation>(picked); changed = true; });

			int blend_src_rgb = static_cast<int>(state.BlendFuncSrcRGB);
			ImGuiExt::DrawComboUI("Blend Src RGB", BlendFuncOptions(), blend_src_rgb,
				[&](int picked) { state.BlendFuncSrcRGB = static_cast<BlendFunc>(picked); changed = true; });

			int blend_src_a = static_cast<int>(state.BlendFuncSrcA);
			ImGuiExt::DrawComboUI("Blend Src A", BlendFuncOptions(), blend_src_a,
				[&](int picked) { state.BlendFuncSrcA = static_cast<BlendFunc>(picked); changed = true; });

			int blend_dst_rgb = static_cast<int>(state.BlendFuncDstRGB);
			ImGuiExt::DrawComboUI("Blend Dst RGB", BlendFuncOptions(), blend_dst_rgb,
				[&](int picked) { state.BlendFuncDstRGB = static_cast<BlendFunc>(picked); changed = true; });

			int blend_dst_a = static_cast<int>(state.BlendFuncDstA);
			ImGuiExt::DrawComboUI("Blend Dst A", BlendFuncOptions(), blend_dst_a,
				[&](int picked) { state.BlendFuncDstA = static_cast<BlendFunc>(picked); changed = true; });

			bool enable_depth_write = state.EnableDepthWrite;
			if (ImGuiExt::DrawCheckboxUI("Depth Write", enable_depth_write))
			{
				state.EnableDepthWrite = enable_depth_write;
				changed = true;
			}

			int depth_compare = static_cast<int>(state.DepthCompareFunc);
			ImGuiExt::DrawComboUI("Depth Test", CompareFuncOptions(), depth_compare,
				[&](int picked) { state.DepthCompareFunc = static_cast<CompareFunc>(picked); changed = true; });

			bool enable_color_write = state.EnableColorWrite;
			if (ImGuiExt::DrawCheckboxUI("Color Write", enable_color_write))
			{
				state.EnableColorWrite = enable_color_write;
				changed = true;
			}

			return changed;
		}

		/* 分区标题行：三角和标题从标签列左沿起排（跟卡片头同一套画法）。不用 stock 树箭头
		 * （它从 FramePadding 起画、会右移一档）；TreeNodeEx 只用来做整行命中和折叠状态，箭头推成
		 * 透明藏掉，这里自绘精确对位 —— 要在 TreeNodeEx 之后立刻调用。 */
		void DrawMaterialSectionHeader(bool open, const char* title)
		{
			const ImVec2 row_min = ImGui::GetItemRectMin();
			const float row_height = ImGui::GetItemRectSize().y;
			const float text_height = ImGui::GetFontSize();
			const float arrow_size = text_height * 0.55f; /* 与卡片头箭头同口径 */
			const float center_y = row_min.y + row_height * 0.5f;

			ImDrawList* draw_list = ImGui::GetWindowDrawList();
			PanelChrome::DrawDisclosureArrow(draw_list,
				ImVec2(row_min.x + arrow_size * 0.5f, center_y),
				arrow_size, open, ImGui::GetColorU32(EditorTheme::Token::TextDim));
			draw_list->AddText(
				ImVec2(row_min.x + arrow_size + ImGui::GetStyle().ItemInnerSpacing.x,
					center_y - text_height * 0.5f),
				ImGui::GetColorU32(EditorTheme::Token::Text), title);
		}

		/* 分区隔离线：把 RasterState 分区与上面的参数行隔开（收起时也在）。
		 * 自绘细线（项目惯例：自绘不占行距、间距说得清；ImGui::Separator 吃「1px + 整行距」、
		 * 且 Separator 色在卡底上偏淡）——块高 = 上留 5 + 线 1 + 下留 5。 */
		void DrawMaterialSectionDivider()
		{
			const float top_gap = 5.0f;
			const float bottom_gap = 5.0f;
			const ImVec2 line_start = ImGui::GetCursorScreenPos();
			const float line_y = line_start.y + top_gap + 0.5f;
			ImGui::GetWindowDrawList()->AddLine(
				ImVec2(line_start.x, line_y),
				ImVec2(line_start.x + ImGui::GetContentRegionAvail().x, line_y),
				ImGui::GetColorU32(EditorTheme::Token::Border));
			ImGui::Dummy(ImVec2(0.0f, top_gap + 1.0f + bottom_gap));
		}
	}

	/* 组件自定义编辑的合并窗口封口（每帧调用；与 DrawEditableField 同一套事务规则）：
	 * 上一帧有控件活跃、这一帧没有 → 一次连续编辑结束，给命令栈封口。 */
	void SceneHierarchy::ServiceComponentEditTransaction()
	{
		const bool item_active = ImGui::IsAnyItemActive();
		if (m_ComponentEditTransactionOpen && !item_active)
		{
			m_ComponentEditTransactionOpen = false;
			if (m_pCommandStack != nullptr)
				m_pCommandStack->EndTransaction();
		}
	}

	/* 材质卡主体：Shader 行 + 参数行（可就地编辑 → 自动实例化） */
	bool SceneHierarchy::DrawMaterialCardBody(ModelComponent& component, const Model& model, int slot_index)
	{
		bool changed = false;

		const SharedPtr<Material> current = ResolveSlotMaterial(model, slot_index, &component.m_SlotOverrides);

		/* Shader 行（只读）：长路径只裁不换行（换行会撑成两行、错位）。裁剪必须走
		 * DrawClippedTextLine（定宽占位 + 省略号）—— PushClipRect 只压绘制不压布局，超宽路径
		 * 会把面板撑出横滑。 */
		if (current->GetShader() != nullptr)
		{
			const float value_width = ImGuiExt::BeginPropertyRow("Shader", EditorTheme::Token::PropertyLabelWidth, true);
			const std::string shader_path = RELATIVE_PATH(current->GetShader()->GetPath());
			PanelChrome::DrawClippedTextLine(shader_path.c_str(), value_width);
			ImGuiExt::EndPropertyRow();
		}

		/* 参数行：集合与类型来自 Shader 反射（见 BuildMaterialParamRows）——换 Shader 行集跟着换；
		 * 值取当前生效材质，反射声明而材质还没有的先显示默认值。每轮重新解析材质（上一轮改动可能已换成实例） */
		const std::vector<MaterialParamInfo> rows = BuildMaterialParamRows(*current);
		if (rows.empty())
			DrawMaterialReflectionNotice(*current);

		for (const MaterialParamInfo& row : rows)
		{
			const SharedPtr<Material> live = ResolveSlotMaterial(model, slot_index, &component.m_SlotOverrides);
			const auto& parameters = live->GetAllParameters();
			const auto iter = parameters.find(ToID(row.Name));
			const MaterialParamInfo& param_info = (iter != parameters.end()) ? iter->second : row;

			DrawMaterialParamRow(param_info, m_AssetRevealFunc,
				[&component, &model, slot_index, &changed](ParamType type, const std::string& name, const std::any& value)
				{
					WriteBackMaterialParam(component, model, slot_index, type, name, value, changed);
				});
		}

		/* 光栅化状态：可折叠分区（默认收起；值画在临时副本上，改动时才实例化写回）。上方压一条
		 * 隔离线。注意：TreeNodeEx 只承担命中 / 折叠状态（标签传空串、三角自绘）；
		 * NoTreePushOnOpen：内容不缩进（默认 TreePush 会 Indent 18px、整段右移错位）。 */
		DrawMaterialSectionDivider();
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 0.0f)); /* 藏掉 stock 箭头（自绘对位） */
			const bool raster_open = ImGui::TreeNodeEx("##MaterialRasterState",
				ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen, "%s", "");
			ImGui::PopStyleColor();
			DrawMaterialSectionHeader(raster_open, "Raster State");

			if (raster_open)
			{
				const SharedPtr<Material> live = ResolveSlotMaterial(model, slot_index, &component.m_SlotOverrides);
				RenderRasterState state = live->GetRasterState();
				if (DrawMaterialRasterStateRows(state))
					WriteBackMaterialRasterState(component, model, slot_index, state, changed);
			}
		}

		return changed;
	}

	/* Model 的材质卡：每槽一张，排在 Model 组件卡之后同级平铺。 */
	void SceneHierarchy::DrawModelMaterialCards(Entity entity, ModelComponent& component)
	{
		PROFILE_FUNCTION();

		if (component.m_Model == nullptr)
			return;

		const auto& slots = component.m_Model->GetMaterialSlots();
		if (slots.empty())
			return;

		const ComponentDesc* desc = ComponentRegistry::Instance().Find(std::type_index(typeid(ModelComponent)));
		if (desc == nullptr)
			return;

		for (int slot_index = 0; slot_index < static_cast<int>(slots.size()); ++slot_index)
		{
			const MaterialSlot& slot = slots[slot_index];

			/* 卡片键按槽索引：多槽各一张、折叠状态互不影响（槽名可能重名，不能当 ID） */
			ImGui::PushID(slot_index);

			const std::string title = "Material · " + slot.Name;
			const PanelChrome::Card card = PanelChrome::BeginCard(title.c_str(), Icons::Id::FileMaterial);

			if (card.Open)
			{
				/* 与 DrawComponentBlock 同一套撤销语义：快照 + 事务合并 */
				const bool can_record = m_pCommandStack != nullptr
					&& desc->Capture != nullptr && desc->Restore != nullptr;

				ComponentSnapshot before;
				if (can_record)
					desc->Capture(entity, before);

				const bool changed = DrawMaterialCardBody(component, *component.m_Model, slot_index);

				if (changed && can_record && before.IsValid())
				{
					if (ImGui::IsAnyItemActive() && !m_ComponentEditTransactionOpen)
					{
						m_ComponentEditTransactionOpen = true;
						m_pCommandStack->BeginTransaction();
					}

					ComponentSnapshot after;
					desc->Capture(entity, after);
					if (after.IsValid())
					{
						m_pCommandStack->Execute(CreateUniquePtr<ComponentCustomDrawCommand>(
							entity, desc->Has, desc->Restore, std::move(before), std::move(after), desc->Name));
					}
				}
			}

			PanelChrome::EndCard(card);
			ImGui::PopID();
		}
	}

	/* ==================== 材质资产编辑卡（.mtl 的资源详情）====================
	 * 资源浏览器选中 .mtl 时画在详情卡后面（每条目一张）。编辑只落编辑缓冲，点了 Apply 才
	 * 序列化回文件 —— 没保存的改动不影响场景（Model 槽位读的是加载时那份）。 */

	/* 卡身：Shader 下拉（按目录分组的子菜单）+ 参数行 + 光栅化状态行（与 Model 材质卡共用控件）。
	 * on_edit：任何改动走这里 —— 调用方记脏并做热更新（同步缓存里的共享材质）。 */
	void SceneHierarchy::DrawAssetMaterialBody(Material& material, const std::function<void()>& on_edit)
	{
		PROFILE_FUNCTION();

		/* Shader 行：值 = 一枚下拉按钮（显示当前 Shader 的文件名、右缘一枚朝下的箭头；
		 * 悬停看全路径）；点开的分组菜单按目录做子菜单（文件夹先、文件后），点一条即选中。
		 * 还没有 Shader 的条目按钮上是占位文案，菜单照常能开、选一个即可补上。 */
		const SharedPtr<DeviceShader> shader = material.GetShader();
		const std::string current = (shader != nullptr) ? RELATIVE_PATH(shader->GetPath()) : std::string();

		const float value_width = ImGuiExt::BeginPropertyRow("Shader", EditorTheme::Token::PropertyLabelWidth, true);

		/* 菜单开着的按钮提亮一档（与组合框打开时的状态语言一致） */
		const bool menu_open = ImGui::IsPopupOpen("##ShaderMenu");
		if (menu_open)
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
		const bool picker_clicked = ImGui::Button("##ShaderPicker", ImVec2(value_width, 0.0f));
		if (menu_open)
			ImGui::PopStyleColor();

		if (picker_clicked)
			ImGui::OpenPopup("##ShaderMenu");

		/* 按钮的自绘内容：文件名居中、右缘一枚朝下的箭头（与 Button 自身同一套
		 * RenderTextClipped 画法 —— 长名字按按钮宽度裁剪，不撑破行） */
		{
			const ImGuiStyle& style = ImGui::GetStyle();
			const ImVec2 min = ImGui::GetItemRectMin();
			const ImVec2 max = ImGui::GetItemRectMax();

			const std::string display = current.empty()
				? std::string(kNoShaderOption)
				: PathToUtf8(PathFromUtf8(current).filename());

			const float arrow_size = ImGui::GetFontSize() * 0.55f;
			const float arrow_zone = arrow_size + style.FramePadding.x * 2.0f;

			const ImVec2 text_size = ImGui::CalcTextSize(display.c_str());
			const ImRect clip(min, max);
			ImGui::RenderTextClipped(ImVec2(min.x + style.FramePadding.x, min.y + style.FramePadding.y),
				ImVec2(max.x - arrow_zone, max.y - style.FramePadding.y),
				display.c_str(), nullptr, &text_size, ImVec2(0.5f, 0.5f), &clip);

			PanelChrome::DrawDisclosureArrow(ImGui::GetWindowDrawList(),
				ImVec2(max.x - style.FramePadding.x - arrow_size * 0.5f, (min.y + max.y) * 0.5f),
				arrow_size, true, ImGui::GetColorU32(EditorTheme::Token::TextDim));
		}

		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", current.empty() ? "Pick a shader" : current.c_str());

		/* 分组菜单：清单很小，打开的每帧现拼一棵树（Apply 后面板重载缓冲，树自然跟着新清单走）。
		 * 弹层给一条最小宽度约束：各行铺满弹层，勾选列因此对齐在同一条纵线上。 */
		ImGui::SetNextWindowSizeConstraints(ImVec2(200.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
		if (ImGui::BeginPopup("##ShaderMenu"))
		{
			/* 菜单打开时重扫一遍可选 Shader：清单平时只在"选中材质 / 材质文件变化"时刷新，
			 * 新建的 .glsl 不必等这些时机 —— 一开菜单就能选到 */
			if (ImGui::IsWindowAppearing())
				m_AssetMaterialShaderOptions = CollectShaderOptions();

			std::vector<ShaderMenuNode> nodes = BuildShaderMenuTree(m_AssetMaterialShaderOptions);

			/* 清单都窝在同一层壳里时（如 Assets/Shaders/…）把空壳脱掉，顶层直接放内容。
			 * 注意：先搬到局部再赋回 —— children 住在 nodes 自己的缓冲里，直接自引用 move 行为未定义
			 * （实测会得到空树）。 */
			if (nodes.size() == 1 && nodes[0].IsFolder())
			{
				std::vector<ShaderMenuNode> inner = std::move(nodes[0].Children);
				nodes = std::move(inner);
			}

			/* 当前 Shader 不在清单里（资产外路径之类）：树顶补一条，选中态照常能看到 */
			if (!current.empty()
				&& std::find(m_AssetMaterialShaderOptions.begin(), m_AssetMaterialShaderOptions.end(), current)
					== m_AssetMaterialShaderOptions.end())
				nodes.insert(nodes.begin(), ShaderMenuNode{ current, current, {} });

			std::string picked;
			DrawShaderMenuLevel(nodes, current, picked);
			ImGui::EndPopup();

			if (!picked.empty() && picked != current)
			{
				material.SetShader(ShaderAssetManager::Instance().GetOrLoad(ABSOLUTE_PATH(picked)));
				on_edit();
			}
		}

		ImGuiExt::EndPropertyRow();

		/* 参数行：集合来自 Shader 反射（换 Shader 后立即按新 Shader 列行）；值取编辑缓冲，
		 * 反射声明而缓冲里还没有的先显示反射默认值 —— 编辑时才落进缓冲；
		 * 反射不到时画报错提示，不回退旧参数表 */
		const std::vector<MaterialParamInfo> rows = BuildMaterialParamRows(material);
		if (rows.empty())
			DrawMaterialReflectionNotice(material);

		for (const MaterialParamInfo& param_info : rows)
		{
			DrawMaterialParamRow(param_info, m_AssetRevealFunc,
				[&material, &on_edit](ParamType type, const std::string& name, const std::any& value)
				{
					if (type == ParamType::Texture)
						material.SetTexture(name, std::any_cast<SharedPtr<DeviceTexture>>(value));
					else
						material.SetParameters(type, name, value);

					/* 有改动 → 记脏（Apply 变可用）+ 热更新（见 DrawAssetMaterialCards） */
					on_edit();
				});
		}

		/* 光栅化状态：可折叠分区（默认收起；值画在临时副本上，改动时才实例化写回）。上方压一条
		 * 隔离线。注意：TreeNodeEx 只承担命中 / 折叠状态（标签传空串、三角自绘）；
		 * NoTreePushOnOpen：内容不缩进（默认 TreePush 会 Indent 18px，跟参数行错位）。 */
		DrawMaterialSectionDivider();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 0.0f)); /* 藏掉 stock 箭头（自绘对位） */
		const bool raster_open = ImGui::TreeNodeEx("##MaterialRasterState",
			ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen, "%s", "");
		ImGui::PopStyleColor();
		DrawMaterialSectionHeader(raster_open, "Raster State");
		if (raster_open)
		{
			if (DrawMaterialRasterStateRows(material.GetRasterState()))
				on_edit();
		}
	}

	/* 每条目一张卡：可编辑的条目（内嵌定义）= 卡身 + 「Apply」；引用形态 / 空条目明确说清是什么，
	 * 而不是给一张空卡。 */
	void SceneHierarchy::DrawAssetMaterialCards(const std::filesystem::path& absolute_path)
	{
		PROFILE_FUNCTION();

		if (m_AssetMaterialGroup == nullptr)
			return;

		const std::string asset_path = PathToUtf8(absolute_path);
		const auto& entries = m_AssetMaterialGroup->GetEntries();
		for (size_t index = 0; index < entries.size(); ++index)
		{
			const MaterialEntry& entry = entries[index];

			/* 卡片键按索引：多条目各一张、折叠状态互不影响（槽名可空、可重名，不能当 ID） */
			ImGui::PushID(static_cast<int>(index));

			std::string title = "Material";
			if (entries.size() > 1)
				title += " " + std::to_string(index);
			if (!entry.SlotName.empty())
				title += " · " + entry.SlotName;

			const PanelChrome::Card card = PanelChrome::BeginCard(title.c_str(), Icons::Id::FileMaterial);
			if (card.Open)
			{
				if (entry.pMaterial != nullptr)
				{
					/* 编辑缓冲的改动即时热更：同步缓存里的共享材质（场景在渲染的那份）——
					 * 场景窗口立刻看到结果，无需等 Apply（Apply 只管落盘）。
					 * 资产约定单条目：只有第 0 条对应对缓存条目。 */
					const SharedPtr<Material> editable = entry.pMaterial;
					const bool hot_sync = (index == 0);
					DrawAssetMaterialBody(*entry.pMaterial, [this, asset_path, editable, hot_sync]()
					{
						m_AssetMaterialDirty = true;
						if (hot_sync)
							MaterialAssetManager::Instance().Refresh(asset_path, editable);
					});

					/* 「Apply」：保存修改并序列化到本地（整份文件的全部条目一起写出）；
					 * 没有改动时禁用 —— 点了也只是原样重写，没有意义 */
					ImGui::Spacing();
					const bool has_changes = m_AssetMaterialDirty;
					if (!has_changes)
						ImGui::BeginDisabled();
					if (ImGui::Button("Apply", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)))
						ApplyAssetMaterialEdits(absolute_path);
					if (!has_changes)
						ImGui::EndDisabled();
					if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
						ImGui::SetTooltip(has_changes ? "Save changes to this .mtl file" : "No changes to apply");
				}
				else if (!entry.AssetPath.empty())
				{
					/* 引用形态：定义在别处（独立材质资产）—— 要改它得选中那份资产 */
					ImGuiExt::DrawCommonTextUI("Asset", RELATIVE_PATH(entry.AssetPath));
					ImGuiExt::DrawCommonTextUI("Type", "Shared material asset");
				}
				else
				{
					ImGuiExt::DrawCommonTextUI("Status", "Invalid material entry");
				}
			}

			PanelChrome::EndCard(card);
			ImGui::PopID();
		}
	}

	/* 「Apply」：保存修改并序列化到本地。
	 * 独立材质资产（约定单条目）另做缓存同步：命中缓存时就地换定义 —— 场景里已引用它的
	 * 槽位无需重载场景即可生效；模型伴生的槽表以文件为准，模型下次加载时读到新定义。 */
	void SceneHierarchy::ApplyAssetMaterialEdits(const std::filesystem::path& absolute_path)
	{
		PROFILE_FUNCTION();

		if (m_AssetMaterialGroup == nullptr)
			return;

		/* 参数表按当前 Shader 收敛后再落盘（反射是集合的单一来源：面板看不到的不写进文件） */
		for (const MaterialEntry& entry : m_AssetMaterialGroup->GetEntries())
		{
			if (entry.pMaterial != nullptr)
				entry.pMaterial->RetainShaderDeclaredParams();
		}

		const std::string path = PathToUtf8(absolute_path);
		m_AssetMaterialGroup->Serializer(path);

		/* 缓冲已与文件一致：按钮回到禁用（下一帧文件写入时间变化触发的重读也会再清一遍） */
		m_AssetMaterialDirty = false;

		if (const MaterialEntry* first = m_AssetMaterialGroup->GetEntryByIndex(0);
			first != nullptr && first->pMaterial != nullptr)
			MaterialAssetManager::Instance().Refresh(path, first->pMaterial);
	}

	/* 层级里拖入 .mesh：给实体挂 / 换模型。
	 * 无组件先补组件（AddComponentCommand）、再设模型（组件快照命令）——两步都可撤销。 */
	void SceneHierarchy::ApplyDroppedMeshToEntity(Entity entity, const std::string& absolute_path)
	{
		PROFILE_FUNCTION();

		SharedPtr<Model> loaded_model = Model::Create(absolute_path);
		if (loaded_model == nullptr)
			return;

		const ComponentDesc* desc = ComponentRegistry::Instance().Find(std::type_index(typeid(ModelComponent)));
		if (desc == nullptr)
			return;

		if (!entity.HasComponent<ModelComponent>())
		{
			if (m_pCommandStack != nullptr && desc->Add != nullptr)
				m_pCommandStack->Execute(CreateUniquePtr<AddComponentCommand>(entity, desc->Name, desc->Add, desc->Remove));
			else
				entity.AddComponent<ModelComponent>();
		}

		/* 设模型：经组件快照命令入栈（与属性面板的 Replace .mesh 同一条撤销机制） */
		const bool can_record = m_pCommandStack != nullptr && desc->Capture != nullptr && desc->Restore != nullptr;
		ComponentSnapshot before;
		if (can_record)
			desc->Capture(entity, before);

		entity.GetComponent<ModelComponent>().m_Model = std::move(loaded_model);

		if (can_record && before.IsValid())
		{
			ComponentSnapshot after;
			desc->Capture(entity, after);
			if (after.IsValid())
			{
				m_pCommandStack->Execute(CreateUniquePtr<ComponentCustomDrawCommand>(
					entity, desc->Has, desc->Restore, std::move(before), std::move(after), desc->Name));
			}
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
			/* 组件自定义编辑的合并窗口封口（拖动材质参数等 → 每帧快照命令由事务窗口合并为一条） */
			ServiceComponentEditTransaction();

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
					/* 用户正在操作控件时开合并窗口：连续拖动只留一条历史 */
					if (ImGui::IsAnyItemActive() && !m_ComponentEditTransactionOpen)
					{
						m_ComponentEditTransactionOpen = true;
						m_pCommandStack->BeginTransaction();
					}

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

			/* Model 的材质卡：每槽一张，排在组件卡之后同级平铺
			 * （卡片背景走绘制通道、通道不可嵌套 —— 详情卡画不进组件卡里）。 */
			if (desc.Type == std::type_index(typeid(ModelComponent)))
				DrawModelMaterialCards(m_SelectedEntity, *static_cast<ModelComponent*>(component));
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
