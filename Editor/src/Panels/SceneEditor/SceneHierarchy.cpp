#include "Pch.h"
#include "SceneHierarchy.h"
#include "SceneHierarchyShared.h"
#include "Common/EditorCommon.h"
#include "UI/EditorIcons.h"
#include "Command/AssetFileOps.h"
#include "UI/PanelChrome.h"
#include "UI/PanelRegistry.h"
#include "Panels/PanelWindow.h"
#include "Helios/Core/AssetManager.h"
#include "Helios/Common/PathUtils.h"
#include "Helios/Common/Utils.h"
#include "Helios/Scene/SceneCommon.h"
#include "Helios/Scene/ReflectionProbe.h"	/* .probe 详情的烘焙结果预览（BuildPreviewTexture） */
#include "EntityTemplateRegistry.h"
#include "Command/CreateEntityCommand.h"
#include "Command/DeleteEntityCommand.h"
#include "Command/ReparentEntityCommand.h"
#include "Command/SetEntityVisibilityCommand.h"
#include "UI/EditorTheme.h"
#include "Helios/Scene/Components.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <unordered_map>
#include <vector>

namespace Helios
{
	using SceneHierarchyShared::ResolveEntityIcon;
	using SceneHierarchyShared::DrawVisibilityToggle;
	using SceneHierarchyShared::SetEntityVisibility;

	namespace
	{
		/* 层级面板内部的拖拽载荷类型：被拖动实体的句柄 */
		constexpr const char* kEntityDragPayload = "SCENE_HIERARCHY_ENTITY";

		/* ---- 树的展开状态 ----
		 * 键 = 从根往下逐级滚出来的名字路径哈希（FNV-1a）：实体句柄会随增删实体漂移，
		 * 名字路径跨场景、跨会话都对得上（同一父链下同名的两个节点共用一份状态，无害）。 */
		constexpr uint64_t kTreeRootKey = 0x9e3779b97f4a7c15ull;

		uint64_t HashTreeKey(uint64_t parent_key, const std::string& name)
		{
			uint64_t hash = parent_key ^ 0xcbf29ce484222325ull;
			for (const char character : name)
			{
				hash ^= static_cast<unsigned char>(character);
				hash *= 0x100000001b3ull;
			}

			return hash;
		}

		/* 展开记录：场景路径 -> (名字路径哈希 -> 是否展开)。按场景分段存同一个文件，整份读写。 */
		using TreeStateTable = std::map<std::string, std::map<uint64_t, bool>>;

		/* 文件格式（<项目>/.helios/hierarchy.cfg）：
		 *   [场景路径]
		 *   名字路径哈希(16 进制)=0/1 */
		void ReadTreeStates(const std::filesystem::path& file, TreeStateTable& out)
		{
			std::ifstream stream(file);
			if (!stream)
				return;

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
				if (separator == std::string::npos || scene.empty())
					continue;

				const uint64_t key = std::strtoull(line.substr(0, separator).c_str(), nullptr, 16);
				out[scene][key] = line.substr(separator + 1) != "0";
			}
		}

		void WriteTreeStates(const std::filesystem::path& file, const TreeStateTable& states)
		{
			std::error_code error;
			std::filesystem::create_directories(file.parent_path(), error);

			std::ofstream stream(file, std::ios::trunc);
			if (!stream)
				return;

			for (const auto& [scene, table] : states)
			{
				if (table.empty())
					continue;

				stream << "[" << scene << "]\n";
				for (const auto& [key, open] : table)
				{
					char buffer[32] = {};
					snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(key));
					stream << buffer << "=" << (open ? 1 : 0) << "\n";
				}
			}
		}

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

		bool HasPostProcessComponent(const Entity& entity)
		{
			return entity.HasComponent<PostProcessComponent>();
		}

		bool HasSkyComponent(const Entity& entity)
		{
			return entity.HasComponent<SkyComponent>();
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
			/* 天空排在模型之后：挂了模型的实体保留模型造型 */
			{ Icons::Id::Sky,              &HasSkyComponent },
			/* 后处理对象排在表尾：挂了模型的实体保留模型造型（追加组件不改既有图标） */
			{ Icons::Id::PostProcess,      &HasPostProcessComponent },
		};

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

		/* 大小写无关的子串匹配（ToLowercase / ContainsCaseInsensitive）与树行内容
		 * （PanelChrome::DrawTreeRowLabel）都走 Editor 侧的共用实现，各面板语义一致。 */

		/* 场景根节点的名字：取文件名（去掉 .scn 后缀），刚新建还没存过就是 Untitled */
		std::string MakeSceneDisplayName(const std::string& path)
		{
			if (path.empty())
				return "Untitled";

			return std::filesystem::path(path).stem().string();
		}

		/* 行右端 = 可视区右缘（不含横向滚动量）。InnerRect 扣掉了滚动条，再让一格内边距，
		 * 口径与 WorkRect 的右端一致；直接拿 WorkRect 的话，滚一下眼睛就跟着内容跑到中间。 */
		inline float TreeRowRight()
		{
			const ImGuiWindow* window = ImGui::GetCurrentWindowRead();
			return window->InnerRect.Max.x - ImMax(window->WindowPadding.x, window->WindowBorderSize);
		}

		/* 实体行内容：按实体持有的组件解析图标，再交给共用的树行版式。
		 * icon_color / clip_right 由调用方算好透传：隐藏行的整体压暗（含图标），
		 * 以及"给行右端的眼睛让出一列"的文本裁剪；content_inset 就是眼睛那一列的宽。 */
		void DrawEntityRowLabel(const Entity& entity, const std::string& name,
		                        ImVec4 icon_color, float clip_right, float content_inset)
		{
			PanelChrome::DrawTreeRowLabel(ResolveEntityIcon(entity), name, false, icon_color,
				clip_right, content_inset);
		}

		/* 栏与主体之间那条分隔线（1px，自绘） */
		inline float kBarDivider() { return Scaled(1.0f); }

		/* 栏里控件"上下各留 1px"：控件框不跟面板边、也不跟那条分隔线贴在一起
		 * （顶栏 / 底栏共用同一笔账，栏高因此比控件高 2px）。 */
		inline float kBarInset() { return Scaled(1.0f); }

		/* 底栏（控件高 + 上下各 1px + 上面那条分隔线）的总高：
		 * 实体树按它让出位置，底栏自己也按它定位 —— 一处算，两处用，对得上。 */
		inline float FooterBandHeight()
		{
			return ImGui::GetFrameHeight() + kBarInset() * 2.0f + kBarDivider();
		}
	}

	/* 层级树与属性面板共用：实体图标、可见性开关（声明见 SceneHierarchyShared.h） */
	namespace SceneHierarchyShared
	{
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

		/* 可见性开关（眼睛）按钮本体：命中区 + 悬停底色 + 图标 + tooltip。层级树行右端与属性
		 * 面板标题栏左端共用（reveal_icon = true 时常驻）。注意：主题给 framed 控件设了 1px 边框，
		 * 透明填充的按钮要把边框推掉，不然常态下会留一圈空心方框。 */
		bool DrawVisibilityToggle(const char* str_id, const ImVec2& rect_min, const ImVec2& rect_max,
		                          bool hidden, bool reveal_icon)
		{
			ImGui::SetCursorScreenPos(rect_min);
			ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
			ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::Token::Clear);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::Token::Clear);
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, EditorTheme::Token::Clear);
			const bool pressed = ImGui::Button(str_id,
				ImVec2(rect_max.x - rect_min.x, rect_max.y - rect_min.y));
			const bool hovered = ImGui::IsItemHovered();
			const bool held = ImGui::IsItemActive();
			ImGui::PopStyleColor(3);
			ImGui::PopStyleVar();

			ImDrawList* const draw_list = ImGui::GetWindowDrawList();

			if (hovered || held)
			{
				draw_list->AddRectFilled(rect_min, rect_max,
					ImGui::GetColorU32(held
						? EditorTheme::WithAlpha(EditorTheme::Token::Neutral7, 0.70f)
						: EditorTheme::WithAlpha(EditorTheme::Token::Neutral6, 0.85f)),
					ImGui::GetStyle().FrameRounding);
			}

			if (hidden || reveal_icon || hovered)
			{
				const ImVec2 center((rect_min.x + rect_max.x) * 0.5f, (rect_min.y + rect_max.y) * 0.5f);
				Icons::Draw(draw_list, hidden ? Icons::Id::Hidden : Icons::Id::Visible,
					center, (rect_max.y - rect_min.y) * 0.66f,
					ImGui::GetColorU32(hovered ? EditorTheme::Token::Text : EditorTheme::Token::TextDim));
			}

			if (hovered)
				ImGui::SetTooltip(hidden ? "Show entity" : "Hide entity");

			return pressed;
		}

		/* 相机不受影响；探针隐藏后不烘焙、也不参与 IBL */
		void SetEntityVisibility(const SharedPtr<Scene>& scene, CommandStack* command_stack,
			Entity entity, bool visible)
		{
			PROFILE_FUNCTION();

			if (scene == nullptr || !scene->IsEntityValid(entity))
				return;

			/* 无命令栈时也要走同一份应用逻辑：命令自带"补组件 / 去组件"的完整语义 */
			auto command = CreateUniquePtr<SetEntityVisibilityCommand>(scene, entity, visible);
			if (command_stack != nullptr)
				command_stack->Execute(std::move(command));
			else
				command->Do();
		}
	}

	SceneHierarchy::SceneHierarchy()
	{
		PROFILE_FUNCTION();

		BindTreeState();
	}

	SceneHierarchy::SceneHierarchy(const SharedPtr<Scene>& scene)
	{
		PROFILE_FUNCTION();

		BindTreeState();
		SetOwnerScene(scene);
	}

	void SceneHierarchy::SetOwnerScene(const SharedPtr<Scene>& scene)
	{
		m_pOwnerScene = scene;
		m_SelectedEntity = {};
		m_Inspector.SetOwnerScene(scene);

		/* 上一份场景里的挂接请求指向的句柄已经无效 */
		m_PendingReparent = {};

		/* 上一份场景画过的树行也不再有效 */
		m_TreeRows.clear();
		m_HasOpenBranch = false;
	}

	void SceneHierarchy::SetScenePath(const std::string& path)
	{
		if (m_ScenePath != path)
		{
			/* 换场景：先把上一份的改动落盘，再把这一份接上（同一路径反复设置是空操作） */
			StoreTreeStates();
			m_TreeRows.clear();
		}

		m_ScenePath = path;
		BindTreeState();
	}

	void SceneHierarchy::SetTreeStatePath(const std::string& path)
	{
		m_TreeStatePath = path;
		LoadTreeStates();
	}

	void SceneHierarchy::BindTreeState()
	{
		/* 指向状态表里的那一份：std::map 的引用稳定，换场景才重新取 */
		m_pTreeState = &m_TreeStates[m_ScenePath];
	}

	void SceneHierarchy::LoadTreeStates()
	{
		if (m_TreeStatesLoaded || m_TreeStatePath.empty())
			return;

		m_TreeStatesLoaded = true;
		ReadTreeStates(PathFromUtf8(m_TreeStatePath), m_TreeStates);
		BindTreeState();
	}

	void SceneHierarchy::StoreTreeStates()
	{
		if (!m_TreeStateDirty)
			return;

		m_TreeStateDirty = false;
		if (m_TreeStatePath.empty())
			return;

		WriteTreeStates(PathFromUtf8(m_TreeStatePath), m_TreeStates);
	}

	void SceneHierarchy::OnImGuiRender()
	{
		PROFILE_FUNCTION();

		/* 场景实体列表 */
		ShowSceneHierarchyUI();

		/* 句柄非空 ≠ 实体存在（见 Entity::operator bool 的约定）：选中项可能来自撤销后的残留、
		 * 失效的拾取句柄，或刚在树里连同祖先一起删掉。组件 GetPtr 对不存在的实体是断言而不是
		 * 返回空指针，所以交给属性面板之前先兜一道。 */
		if (m_SelectedEntity && (m_pOwnerScene == nullptr || !m_pOwnerScene->IsEntityValid(m_SelectedEntity)))
			m_SelectedEntity = {};

		/* 实体属性 */
		m_Inspector.OnImGuiRender(m_SelectedEntity);
	}

	void SceneHierarchy::SetSelectedEntity(const Entity& entity)
	{
		m_SelectedEntity = entity;

		/* 最近一次选择说了算：点实体（层级 / 视口）把属性面板切回组件视图 */
		m_Inspector.ClearAssetFocus();
	}

	void SceneHierarchy::ShowSceneHierarchyUI()
	{
		PROFILE_FUNCTION();

		/* 删除请求延后到遍历结束后执行：在 each 中销毁当前实体并不安全 */
		Entity pending_delete{};

		/* 逐帧记账从这里重来：本帧画到的行既决定底栏按钮能收起 / 展开什么，也是自检要量的行 */
		m_TreeRows.clear();
		m_HasOpenBranch = false;
		m_LastRowRects.clear();

		/* 一个面板 = 顶栏（搜索 + 类型筛选 + 新建）+ 实体树（子窗口，自己滚）+ 底栏（实体计数）。
		 * 面板本身不带上下内边距：顶栏贴面板上边、底栏贴下边（栏高由栏自己说了算）；
		 * 左右内边距保留，顶栏控件与树的每一行对得上同一条竖线。 */
		const ImVec2 pane_padding = ImGui::GetStyle().WindowPadding;
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pane_padding.x, 0.0f));

		/* 被别的页签挡住或折叠时不建树也不画 */
		if (PanelWindow window(Panel::kSceneHierarchy); window)
		{
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

			/* 弹层 ID 在面板根作用域上算一次：按钮（开）与菜单（画）的作用域必须一致，
			 * 两边才按同一个 ID 对接。 */
			m_PopupNewEntity = ImGui::GetID("##NewEntityMenu");
			m_PopupTypeFilter = ImGui::GetID("##TypeFilterMenu");

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

			/* 树再吃一档右内边距：右内沿贴死面板右缘，滚动条才贴着边画（不然停在离边界两档内边距的地方）。
			 * 绘制裁剪会向内缩半格内边距，用树自己的矩形顶掉当前裁剪，滚动条最右几像素才不会被裁掉。 */
			const ImVec2 tree_pos = ImGui::GetCursorScreenPos();
			const ImVec2 tree_size(ImGui::GetContentRegionAvail().x + pane_padding.x, body_height);
			ImGui::PushClipRect(tree_pos, ImVec2(tree_pos.x + tree_size.x, tree_pos.y + tree_size.y), false);
			ImGui::BeginChild("##HierarchyTree", tree_size);
			{
				/* 行的高亮与命中区都按 WorkRect 算，而它会跟着横向滚动往左收 ——
				 * 滚一下行右端就离窗口右缘差一截，眼睛那一列落在行外。把右端拉回可视区。 */
				ImGuiWindow* tree = ImGui::GetCurrentWindow();
				tree->WorkRect.Max.x += tree->Scroll.x;

				/* 场景名作为根节点，实体树挂在它下面 */
				ShowSceneRootNode(roots, children, pending_delete, filter);

				/* 条目之下的空白区域：拖到这里表示提升到根层级。
				 * 只覆盖空白、不覆盖条目：落在条目上的拖拽目标因为"源与目标相同"会被
				 * ImGui 拒绝（拖起来又原位放下），这时若整窗也算目标，就会误当成提升到根层级。 */
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
			ImGui::PopClipRect();
			ImGui::PopStyleVar();

			/* 树是故意越过右沿、顶到面板右缘的 —— 子窗口矩形会被算进面板内容范围，
			 * 面板就多出一截横滑区。把横向内容范围钳回内容区右内沿（纵向不动）。 */
			panel->DC.CursorMaxPos.x = ImMin(panel->DC.CursorMaxPos.x, panel_max.x - pane_padding.x);

			/* 拖到面板外松手：挂到场景根，成为一级节点。判据是"鼠标还在不在面板矩形内" ——
			 * 不能用"有没有拖拽目标接收"（拖回原条目上松手时目标会被 ImGui 拒绝，误判成拖出去了）。 */
			const ImVec2 mouse_pos = ImGui::GetMousePos();
			const bool drop_inside_panel = (mouse_pos.x >= panel_min.x && mouse_pos.x < panel_max.x
				&& mouse_pos.y >= panel_min.y && mouse_pos.y < panel_max.y);
			if (!drop_inside_panel)
				ApplyDropOutsidePanel();

			ShowHierarchyFooter(shown_count, total_count);

			/* 顶栏两枚按钮的下拉菜单（「新建」/「筛选」）都画在面板根作用域
			 * （按钮也是在这一层开的它们），并把主题的窗口内边距补回去
			 * （面板为贴上下边压成了 0 —— 不补的话首末条目紧贴弹层背景的上下边）。 */
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pane_padding);
			DrawNewEntityMenu();
			DrawTypeFilterMenu();
			ImGui::PopStyleVar();
		}

		ImGui::PopStyleVar();

		/* 删除经命令栈落地，可撤销 */
		if (pending_delete)
			DeleteEntity(pending_delete);

		/* 挂接同样延后到这里：遍历中改层级会让本帧的子节点列表失真 */
		ApplyPendingReparent();

		/* 展开状态有改动才落盘（点折叠箭头 / 底栏的收起展开都算） */
		StoreTreeStates();
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
		row.Height += kBarInset() * 2.0f;

		const float control_height = ImGui::GetFrameHeight();
		const float control_y = row.Min.y + kBarInset();
		const float gap = style.ItemInnerSpacing.x;

		/* ---- 右端：筛选 + 新建（两枚图标按钮，筛选在左、新建贴最右）----
		 * 新建图标复用资源浏览器的「新建资源」：图案一样，靠 tooltip 和菜单内容区分。 */
		const float new_entity_x = row.Right - control_height;
		const float filter_x = new_entity_x - gap - control_height;

		const bool filter_active = (m_TypeFilter != TypeFilter::All);
		const std::string filter_tip = filter_active
			? std::string("Filter by entity type: ") + TypeFilterName(m_TypeFilter)
			: std::string("Filter by entity type");

		ImGui::SetCursorScreenPos(ImVec2(filter_x, control_y));
		const bool filter_popup_open = ImGui::IsPopupOpen(m_PopupTypeFilter, ImGuiPopupFlags_None);
		if (Icons::IconButton(Icons::Id::Filter, ImVec2(control_height, control_height),
				filter_active, filter_tip.c_str()) && !filter_popup_open)
		{
			ImGui::OpenPopup(m_PopupTypeFilter);
		}

		/* 弹层锚点 = 按钮右下角（按钮矩形只在这一刻可取）：弹层锚在它正下方、
		 * 右缘对齐 —— 与工具栏的 Debug View / Gizmos 菜单同款锚法。 */
		m_PopupTypeFilterAnchor = ImGui::GetItemRectMax();

		ImGui::SetCursorScreenPos(ImVec2(new_entity_x, control_y));
		if (Icons::IconButton(Icons::Id::NewAsset, ImVec2(control_height, control_height), false,
			"New entity  (empty / 3D / sprite / camera / reflection probe / light)"))
		{
			ImGui::OpenPopup(m_PopupNewEntity);
		}

		/* ---- 左端：搜索框（贴左端）----
		 * 搜索框不撑满：够用就好，宽度多出来的部分留给中间那一段留白；
		 * 空间不足时跟着可用宽度缩，最低 80（再窄就读不了了）。 */
		const float search_preferred = Scaled(220.0f);
		const float search_min = Scaled(80.0f);

		/* 能用的宽度：到右端那组按钮之前算起（中间隔一格） */
		const float right_group = control_height * 2.0f + gap;
		const float group_room = row.Right - right_group - gap - row.Min.x;
		const float search_width = ImMin(search_preferred, ImMax(group_room, search_min));

		ImGui::SetCursorScreenPos(ImVec2(row.Min.x, control_y));
		ImGui::SetNextItemWidth(search_width);
		Icons::BeginSearchInput();
		ImGui::InputTextWithHint("##HierarchyFilter", "Search...",
			m_EntityFilter, sizeof(m_EntityFilter));
		Icons::EndSearchInput();

		PanelChrome::EndHeaderRow(row);
	}

	/* 底栏：实体计数贴行右端（上面一条自绘分隔线、文本垂直居中、上下各留 1px，跟顶栏同一
	 * 笔账）。计数含"为挂住命中项而留下的祖先"—— 它就是屏幕上真能数出来的行数。 */
	void SceneHierarchy::ShowHierarchyFooter(int shown_count, int total_count)
	{
		PROFILE_FUNCTION();

		char count_text[48] = {};
		if (shown_count != total_count)
			snprintf(count_text, sizeof(count_text), "%d / %d", shown_count, total_count);
		else
			snprintf(count_text, sizeof(count_text), "%d entities", total_count);

		ImGuiWindow* window = ImGui::GetCurrentWindow();

		/* 定位：贴面板的下边（分隔线在栏的正上方）。栏高 = 控件高 + 上下各 1px ——
		 * 比控件高 2px 的那点留白与顶栏同一笔账（文本不跟分隔线、也不跟面板下边贴在一起）。 */
		const float pane_bottom = window->Pos.y + window->Size.y;
		const float footer_top = pane_bottom - ImGui::GetFrameHeight() - kBarInset() * 2.0f;
		ImGui::SetCursorScreenPos(ImVec2(window->DC.CursorStartPos.x, footer_top));

		/* 上边线自绘而不是用 ImGui::Separator()：Separator 会吃掉「1px + 行距」，
		 * 于是这一栏就比控件高出一截；自绘的线不参与布局，栏高说得清也量得准。 */
		ImGui::GetWindowDrawList()->AddLine(
			ImVec2(window->DC.CursorStartPos.x, footer_top - kBarDivider() * 0.5f),
			ImVec2(window->DC.CursorStartPos.x + ImGui::GetContentRegionAvail().x,
				footer_top - kBarDivider() * 0.5f),
			ImGui::GetColorU32(EditorTheme::Token::Border));

		const PanelChrome::HeaderRow row = PanelChrome::BeginHeaderRow(Icons::Id::None);
		const float control_height = ImGui::GetFrameHeight();
		const float count_width = ImGui::CalcTextSize(count_text).x;

		/* ---- 左端：一键收起 / 展开 ----
		 * 树里还有展开着的分支就收起全部，全收起了就展开全部（图标跟着动作换）。
		 * 落的是同一份展开状态，收完再打开场景还是收着的；没有可展开的节点时置灰。 */
		ImGui::SetCursorScreenPos(ImVec2(row.Min.x, row.Min.y + kBarInset()));
		ImGui::BeginDisabled(!HasExpandableBranch());

		const bool collapse_all = m_HasOpenBranch;
		if (Icons::IconButton(collapse_all ? Icons::Id::CollapseAll : Icons::Id::ExpandAll,
			ImVec2(control_height, control_height), false,
			collapse_all ? "Collapse all" : "Expand all"))
		{
			SetAllNodesOpen(!collapse_all);
		}

		ImGui::EndDisabled();

		/* ---- 计数贴最右端 ----
		 * 文本自己算居中的 y（Text 把字形框顶放在光标处，"贴着行顶画"会偏高半个内边距）；
		 * 顺带清掉"基线偏移"（同一行前面的控件会把后面的文字整体下移，居中值就被顶掉了）。 */
		const float text_y = row.Min.y + kBarInset() + (control_height - ImGui::GetFontSize()) * 0.5f;
		ImGui::SetCursorScreenPos(ImVec2(row.Right - count_width, text_y));
		window->DC.CurrLineTextBaseOffset = 0.0f;
		ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
		ImGui::TextUnformatted(count_text);
		ImGui::PopStyleColor();

		PanelChrome::EndHeaderRow(row, false);
	}

	/* 底栏按钮：整棵树都走一遍，不只是本帧画出来的那几行 ——
	 * 收起之后深层节点根本没画，只按可见行展开的话点「展开全部」还是只开一层。 */
	void SceneHierarchy::SetAllNodesOpen(bool open)
	{
		if (m_pTreeState == nullptr)
			return;

		EntityChildren children;
		std::vector<Entity> roots;
		BuildHierarchy(children, roots);

		const auto walk = [&](auto&& self, const Entity& entity, uint64_t parent_key) -> void
		{
			const auto child_entry = children.find(entity);
			if (child_entry == children.end() || child_entry->second.empty())
				return;

			const auto& name = entity.GetComponent<NameComponent>().m_Name;
			const uint64_t key = HashTreeKey(parent_key, name);
			(*m_pTreeState)[key] = open;

			for (const Entity& child : child_entry->second)
				self(self, child, key);
		};

		for (const Entity& root : roots)
			walk(walk, root, kTreeRootKey);

		m_TreeStateDirty = true;
	}

	bool SceneHierarchy::HasExpandableBranch() const
	{
		for (const TreeRowState& row : m_TreeRows)
		{
			if (row.HasChildren)
				return true;
		}

		return false;
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

		ImGui::SetNextItemWidth(Scaled(220.0f));
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

	/* 顶栏「筛选」下拉：类型单选列表（All types + 六类，图标跟树里同一套映射）。点行不
	 * 收起弹层（方便连点切换、留在原地），收起靠点外部 / Esc；弹层锚在按钮正下方、右缘对齐。 */
	void SceneHierarchy::DrawTypeFilterMenu()
	{
		PROFILE_FUNCTION();

		ImGui::SetNextWindowPos(ImVec2(m_PopupTypeFilterAnchor.x, m_PopupTypeFilterAnchor.y + Scaled(4.0f)),
			ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));

		/* 最小宽度取最长行标题 + 图标与勾选列的固定组分（与工具栏两套菜单同一笔账） */
		const ImGuiStyle& style = ImGui::GetStyle();
		const float min_width = ImGui::CalcTextSize("Reflection Probes").x
			+ style.FramePadding.x * 2.0f + style.ItemInnerSpacing.x * 2.0f
			+ ImGui::GetFontSize() * 2.0f + 24.0f;
		ImGui::SetNextWindowSizeConstraints(ImVec2(min_width, 0.0f), ImVec2(FLT_MAX, FLT_MAX));

		/* BeginPopupEx 不会自动加 NoTitleBar（BeginPopup 才加）——漏了弹层顶上
		 * 会多出一条空标题栏与折叠钮。 */
		if (!ImGui::BeginPopupEx(m_PopupTypeFilter,
				ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
				| ImGuiWindowFlags_NoTitleBar))
			return;

		/* 行距对齐工具栏菜单：Debug View / Gizmos 的弹层在自己 PushStyleVar(ItemSpacing, (2,0))
		 * 的作用域里（行贴行）；面板这边是主题默认的 7px —— 不补这一档，行间会显得松。 */
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 0.0f));

		/* 复位行 + 各类型（顺序 = TypeFilter 枚举序，与显示名同一套） */
		if (PanelChrome::MenuItemSelectWithIcon(Icons::Id::Filter, TypeFilterName(TypeFilter::All),
				m_TypeFilter == TypeFilter::All))
			m_TypeFilter = TypeFilter::All;

		ImGui::Separator();

		for (int i = 1; i < static_cast<int>(TypeFilter::COUNT); ++i)
		{
			const auto filter = static_cast<TypeFilter>(i);
			if (PanelChrome::MenuItemSelectWithIcon(FilterIconOf(filter), TypeFilterName(filter),
					filter == m_TypeFilter))
				m_TypeFilter = filter;
		}

		ImGui::PopStyleVar();

		ImGui::EndPopup();
	}

	/* 类型筛选菜单行的图标：与树里各类型用的图标同一套（All = 漏斗本体）——
	 * 菜单与树的"这一项长什么样"永远是同一个答案。 */
	Icons::Id SceneHierarchy::FilterIconOf(TypeFilter filter)
	{
		switch (filter)
		{
		case TypeFilter::All:             return Icons::Id::Filter;
		case TypeFilter::Camera:          return Icons::Id::Camera;
		case TypeFilter::Light:           return Icons::Id::Light;
		case TypeFilter::Model:           return Icons::Id::Model;
		case TypeFilter::Sprite:          return Icons::Id::Sprite;
		case TypeFilter::ReflectionProbe: return Icons::Id::ReflectionProbe;
		case TypeFilter::Other:           return Icons::Id::Entity;
		}

		return Icons::Id::Filter;
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
		PanelChrome::PushMenuStyle();
		if (ImGui::BeginPopupContextItem())
		{
			ShowCreateEntityMenu();
			ImGui::EndPopup();
		}
		PanelChrome::PopMenuStyle();

		/* 行内只显示文件名（够短），完整路径挂在悬停提示里；从未保存过就没有路径可显示 */
		if (!m_ScenePath.empty() && ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", m_ScenePath.c_str());

		/* 裁到行的右端（脏标记再占一小格）；放不下时按整段名字占位，横向能滑出来 */
		const float clip_right = TreeRowRight() - (m_SceneDirty ? ImGui::GetFontSize() : 0.0f);
		PanelChrome::DrawTreeRowLabel(Icons::Id::Scene, MakeSceneDisplayName(m_ScenePath), m_SceneDirty,
			EditorTheme::Token::Text, clip_right);

		if (is_opened)
		{
			for (const Entity& root : roots)
				ShowEntityNode(root, children, pending_delete, filter, kTreeRootKey, false);

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
	                                    Entity& pending_delete, const FilterSet* filter, uint64_t parent_key,
	                                    bool parent_hidden)
	{
		PROFILE_FUNCTION();

		/* 过滤时：不在可见集合里的实体整棵子树都不画 */
		if (filter != nullptr && filter->find(entity) == filter->end())
			return;

		const auto& name = entity.GetComponent<NameComponent>().m_Name;

		/* 可见性两种口径：眼睛画的是自身状态（点它只写自己那一行），
		 * 行的压暗看的是有效状态（自身或祖先被隐藏）。祖先的结果由上一层传下来，不逐行上溯父链。 */
		const auto* visibility = m_pOwnerScene->GetRegistry().try_get<VisibilityComponent>(entity);
		const bool self_hidden = (visibility != nullptr) && !visibility->m_Visible;
		const bool effectively_hidden = parent_hidden || self_hidden;

		const auto child_entry = children.find(entity);
		const std::vector<Entity>* sub_entities = (child_entry != children.end()) ? &child_entry->second : nullptr;
		const bool has_children = (sub_entities != nullptr && !sub_entities->empty());

		/* 这一行在展开状态表里的键；默认只展开一级节点 —— 模型实体的骨骼子链一进来是收着的，
		 * 想展开哪一枝由用户点，点过的结果记下来，下次打开场景照旧。 */
		const uint64_t tree_key = HashTreeKey(parent_key, name);
		bool stored_open = (parent_key == kTreeRootKey);
		if (m_pTreeState != nullptr)
		{
			const auto stored = m_pTreeState->find(tree_key);
			if (stored != m_pTreeState->end())
				stored_open = stored->second;
		}

		/* 过滤时全部展开：命中的实体挂在父链下面，父链不展开就等于没搜到 */
		if (has_children)
			ImGui::SetNextItemOpen((filter != nullptr) ? true : stored_open, ImGuiCond_Always);

		/* SpanFullWidth：高亮撑满整行（连缩进区）。AllowItemOverlap：行右端的可见性开关叠在行
		 * 命中区上面，后提交的先拿到悬停和点击；行自身的选中要显式让开开关那一块。
		 * 开合不设 DefaultOpen：由 SetNextItemOpen 按状态表逐帧定。 */
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth
			| ImGuiTreeNodeFlags_AllowItemOverlap;
		if (m_SelectedEntity == entity)
			flags |= ImGuiTreeNodeFlags_Selected;
		/* 没有子节点就是叶子：不能画成可展开的节点，否则会多出一层空节点 */
		if (!has_children)
			flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;

		/* 悬停选中行时 ImGui 画的也是 HeaderHovered（会盖掉选中色）——
		 * 推一档"更亮的选中色"顶住，见 EditorTheme::RowHoverSelected */
		if (m_SelectedEntity == entity)
			ImGui::PushStyleColor(ImGuiCol_HeaderHovered, EditorTheme::RowHoverSelected);

		/* 行内容起点（缩进后的左沿）：折叠箭头就画在这一段，双击判定要按它让开 */
		const float row_content_x = ImGui::GetCursorScreenPos().x;

		/* 空标签：节点自带的名字文字不画，改为下面自绘"图标 + 名字"。
		 * 必须传空字符串：格式化重载会显式传 label_end，而 ImGui 只在 label_end 为 NULL 时
		 * 才隐藏 "##" 之后的内容。 */
		const bool is_opened = ImGui::TreeNodeEx((void*)(uint64_t)(uint32_t)entity, flags, "%s", "");

		if (m_SelectedEntity == entity)
			ImGui::PopStyleColor();

		/* 记账：本帧画到了这一行（底栏的收起 / 展开全部按它落笔）。
		 * 用户点了折叠箭头就把结果记进状态表 —— 过滤期间是强制展开的，不记。 */
		if (has_children)
		{
			m_TreeRows.push_back({ tree_key, true });
			m_HasOpenBranch = m_HasOpenBranch || is_opened;

			if (filter == nullptr && m_pTreeState != nullptr && is_opened != stored_open)
			{
				(*m_pTreeState)[tree_key] = is_opened;
				m_TreeStateDirty = true;
			}
		}

		/* 行右端给可见性开关留一列：开关的命中区 = 行右端的方框（与行同高）。
		 * 此刻"上一项"还是树节点，行高从它身上取；右端按可视区算，不跟着横向滚动跑。 */
		const ImVec2 row_min = ImGui::GetItemRectMin();
		const ImVec2 row_max = ImGui::GetItemRectMax();
		m_LastRowRects.push_back({ entity, row_min, row_max });
		const float eye_side = ImMax(row_max.y - row_min.y, 1.0f);
		const float row_right = TreeRowRight();
		const ImVec2 eye_min(row_right - eye_side, row_min.y);
		const ImVec2 eye_max(row_right, row_max.y);

		/* 行的悬停（开关还没提交，HoveredId 不会被它抢走）：开关"悬停现形"要看它 */
		const bool row_hovered = ImGui::IsItemHovered();

		/* 点眼睛那一下不算选中这行（开关与选中是两件事）：开关方框里的按下留给开关自己。
		 * 双击行（名字 / 图标那一段）= 让视口镜头定位过去；折叠箭头上那一下留给 ImGui 开合。 */
		const float fold_arrow_right = row_content_x + ImGui::GetFontSize()
			+ ImGui::GetStyle().FramePadding.x * 2.0f;
		if (ImGui::IsItemClicked() && !ImGui::IsMouseHoveringRect(eye_min, eye_max))
		{
			SetSelectedEntity(entity);

			if (m_FocusEntityFunc != nullptr && ImGui::GetMousePos().x >= fold_arrow_right
				&& ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				m_FocusEntityFunc(entity);
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

			/* 资源浏览器拖入 .mesh = 给这个实体挂 / 换模型 */
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RESOURCE_BROWSER_ITEM"))
			{
				std::filesystem::path relative_path;
				if (payload->DataSize > 1 && TryPathFromUtf8Payload(
					payload->Data, static_cast<size_t>(payload->DataSize), relative_path))
				{
					const std::string extension = LowercaseExtension(relative_path);

					if (extension == ".mesh" || extension == ".model")
						m_Inspector.ApplyDroppedMeshToEntity(entity, PathToUtf8(ResolveAssetPath(relative_path)));
				}
			}

			ImGui::EndDragDropTarget();
		}

		/* 右键选择删除当前节点 */
		PanelChrome::PushMenuStyle();
		if (ImGui::BeginPopupContextItem())
		{
			if (PanelChrome::MenuItemText("Delete", "Del"))
				pending_delete = entity;

			ImGui::EndPopup();
		}
		PanelChrome::PopMenuStyle();

		/* 交互都在上面处理完了（那时"上一项"才是树节点），这里补画行内容：
		 * 有效隐藏的行整体压暗（名字与图标），名字裁到开关列之前（放不下用省略号收尾）。
		 * 整块再按开关列的左沿裁一刀：图标也不许压进眼睛那一列。 */
		const ImVec4 row_color = effectively_hidden ? EditorTheme::Token::TextDim : EditorTheme::Token::Text;
		const ImGuiStyle& row_style = ImGui::GetStyle();
		const float eye_left = eye_min.x - row_style.ItemInnerSpacing.x;

		/* 放不下时排版按"整段名字 + 眼睛列"占位：横向滑到底，深节点的名字能整段看全 */
		const float eye_inset = eye_side + row_style.ItemInnerSpacing.x * 2.0f;

		ImGui::PushStyleColor(ImGuiCol_Text, row_color);
		ImGui::PushClipRect(ImVec2(row_min.x, row_min.y), ImVec2(eye_left, row_max.y), true);
		DrawEntityRowLabel(entity, name, row_color, eye_left, eye_inset);
		ImGui::PopClipRect();
		ImGui::PopStyleColor();

		/* ---- 行右端：可见性开关（眼睛）----
		 * 可见的实体悬停才现形（留白优先给名字）；隐藏的实体常驻 —— 划掉的眼睛就是"没画进视口"
		 * 的记号。按钮本体跟属性面板标题栏共用（见 DrawVisibilityToggle）。 */
		ImGui::PushID(static_cast<int>(static_cast<uint32_t>(entity)));
		const bool eye_pressed = DrawVisibilityToggle("##EntityVisibility",
			eye_min, eye_max, self_hidden, /*reveal_icon=*/row_hovered);
		ImGui::PopID();

		/* 点一下 = 翻转到当前的反面（目标值就是"现在的自身状态"） */
		if (eye_pressed)
			SetEntityVisibility(m_pOwnerScene, m_pCommandStack, entity, self_hidden);

		/* 展开子节点（叶子节点没有 TreePush，无需 TreePop） */
		if (has_children && is_opened)
		{
			for (const Entity& child : *sub_entities)
				ShowEntityNode(child, children, pending_delete, filter, tree_key, effectively_hidden);

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
}
