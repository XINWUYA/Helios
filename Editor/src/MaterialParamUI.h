#pragma once
/* 材质参数编辑的共享控件（header-only）：Shader 选择行 + 按 Shader 反射列出的参数行。
 * 属性面板的材质卡（实体槽位 / .mtl 资产）与模型编辑器共用这一份 —— 两处的行控件、
 * 反射口径、Shader 菜单都保持同一种语言。改动只落调用方给的 write_back / 材质本身。 */

#include <algorithm>
#include <cfloat>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>

#include "EditorIcons.h"
#include "PanelChrome.h"
#include "Helios/ImGui/EditorTheme.h"
#include "Helios/ImGui/ImGuiExtensions.h"
#include "Helios/Core/AssetManager.h"
#include "Helios/Scene/Material.h"
#include "Helios/Scene/SceneCommon.h"

namespace Helios::MaterialParamUI
{
	/* 材质没有 Shader 时按钮上的占位文案（菜单照常能开，选一个即可补上） */
	inline constexpr const char* kNoShaderOption = "(no shader)";

	/* 反射参数 → 显示行：材质还没有这条时用它撑起一行 ——
	 * 默认值取 Shader 源码声明的（如 u_AlbedoTilingOffset = vec4(1,1,0,0)），
	 * 没有就按类型给零值；贴图没有"默认"，显示成空槽（拖入才落材质）。 */
	inline MaterialParamInfo MakeDefaultMaterialParamRow(const ReflectedMaterialParam& reflected)
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

	/* 一行 = 当前值 + 这条属性的面板规范（显示名 / 控件类型 / 范围 / 条件） */
	struct MaterialParamRow
	{
		MaterialParamInfo Param;
		MaterialPropertySpec Spec;
	};

	/* @showif 求值：拿材质里的当前值比（没有就用反射默认值）。开关不是 int / float、
	 * 或者那个参数压根没声明时按可见处理 —— 别把行藏没了。 */
	inline bool IsPropertyVisible(const Material& material, const ShaderReflectionData& reflection,
		const MaterialPropertySpec& spec)
	{
		if (spec.ShowIfParam.empty())
			return true;

		std::any value;
		const auto& parameters = material.GetAllParameters();
		const auto iter = parameters.find(ToID(spec.ShowIfParam));
		if (iter != parameters.end())
			value = iter->second.Value;
		else if (const ReflectedMaterialParam* declared = reflection.FindMaterialParam(spec.ShowIfParam))
			value = declared->Default;

		if (value.type() == typeid(int))
			return static_cast<float>(std::any_cast<int>(value)) == spec.ShowIfValue;
		if (value.type() == typeid(float))
			return std::any_cast<float>(value) == spec.ShowIfValue;
		return true;
	}

	/* 材质参数行的显示清单：Shader 反射声明里标了 @property 的才列（反射是参数集合的单一来源；
	 * 没标的是引擎逐绘制给的值或库内部声明），再按 @showif 过滤掉当前不该露面的；值优先取材质里
	 * 已有的，没有的就显示反射默认值。反射不到就返回空、不回退旧参数表（让调用方画报错）。 */
	inline std::vector<MaterialParamRow> BuildMaterialParamRows(const Material& material)
	{
		std::vector<MaterialParamRow> rows;

		const SharedPtr<DeviceShader> shader = material.GetShader();
		if (shader == nullptr)
			return rows;

		const ShaderReflectionData& reflection = shader->GetReflectionData();
		const auto& parameters = material.GetAllParameters();
		rows.reserve(reflection.MaterialParams.size());
		for (const ReflectedMaterialParam& reflected : reflection.MaterialParams)
		{
			/* 引擎自己绑的参数（如 u_ShadowMap / u_BRDFLut）没标 @property，不列：
			 * 贴图由渲染通道逐绘制给，面板改了也会被覆盖 */
			if (!reflected.Property)
				continue;
			if (!IsPropertyVisible(material, reflection, reflected.PropertySpec))
				continue;

			const auto iter = parameters.find(ToID(reflected.Name));
			rows.push_back({ iter != parameters.end() ? iter->second : MakeDefaultMaterialParamRow(reflected),
				reflected.PropertySpec });
		}

		std::sort(rows.begin(), rows.end(),
			[](const MaterialParamRow& lhs, const MaterialParamRow& rhs) { return lhs.Param.Name < rhs.Param.Name; });
		return rows;
	}

	/* 参数行空时的提示行（Danger 色）：三种状态分开说清 —— 没 Shader / 有 Shader 但反射为空
	 * （后端不支持或声明识别不出）/ 反射到了但没有一条标 @property（材质没东西可改）。 */
	inline void DrawMaterialReflectionNotice(const Material& material)
	{
		const SharedPtr<DeviceShader> shader = material.GetShader();
		const char* message = nullptr;
		if (shader == nullptr)
			message = "No shader loaded — cannot reflect parameters";
		else if (shader->GetReflectionData().MaterialParams.empty())
			message = "Shader reflection unavailable — parameters not listed";
		else
			message = "No @property params — nothing to edit in this shader";

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
	inline std::vector<ShaderMenuNode> BuildShaderMenuTree(const std::vector<std::string>& options)
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
	inline void DrawShaderMenuLevel(const std::vector<ShaderMenuNode>& nodes, const std::string& current,
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

	/* 材质下拉的目录白名单：只有这里放"给场景物体选的材质" */
	inline constexpr const char* kShaderMenuDir = "Shaders/MaterialShaders/";

	/* 可选 Shader：只收 MaterialShaders 目录下含 stage 声明的 .glsl（相对路径，字典序）——
	 * 管线内 shader 与库文件都不进清单；跳过 Cache（编译产物目录），整树扫。
	 * 调用方缓存清单，在选中变化 / 菜单打开时重扫。 */
	inline std::vector<std::string> CollectShaderOptions()
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

			const std::string relative = RELATIVE_PATH(PathToUtf8(item.path()));
			if (relative.rfind(kShaderMenuDir, 0) != 0)
				continue;

			/* 目录里也得含 stage 声明（与编译器的切分判据一致），防库文件混进来 */
			bool has_stage = false;
			std::ifstream source(item.path(), std::ios::binary);
			for (std::string line; std::getline(source, line); )
			{
				if (line.find("#type") != std::string::npos)
				{
					has_stage = true;
					break;
				}
			}
			if (!has_stage)
				continue;

			options.emplace_back(relative);
		}

		std::sort(options.begin(), options.end());
		return options;
	}

	/* Shader 选择行（标签 + 下拉按钮 + 按目录分组的选择菜单）：
	 * 按钮上显示当前 Shader 的文件名、右缘一枚朝下的箭头；悬停看全路径；
	 * 菜单打开时重扫可选清单；当前 Shader 不在清单里（资产外路径之类）时树顶补一条。
	 * 返回 true = 选中了新的 Shader（已 SetShader，调用方按需热更）。 */
	inline std::string DrawMaterialShaderPicker(const Material& material, std::vector<std::string>& options_cache)
	{
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

		std::string picked;
		/* 弹层给一条最小宽度约束：各行铺满弹层，勾选列因此对齐在同一条纵线上 */
		ImGui::SetNextWindowSizeConstraints(ImVec2(Scaled(200.0f), 0.0f), ImVec2(FLT_MAX, FLT_MAX));
		if (ImGui::BeginPopup("##ShaderMenu"))
		{
			/* 菜单打开时重扫一遍可选 Shader：平时清单由调用方按需刷新，新建的 .glsl
			 * 不必等那些时机 —— 一开菜单就能选到 */
			if (ImGui::IsWindowAppearing())
				options_cache = CollectShaderOptions();

			std::vector<ShaderMenuNode> nodes = BuildShaderMenuTree(options_cache);

			/* 清单都窝在同一条目录链里时（如 Assets/Shaders/MaterialShaders/…）逐层脱掉空壳，
			 * 顶层直接放内容。注意：先搬到局部再赋回 —— children 住在 nodes 自己的缓冲里，直接
			 * 自引用 move 行为未定义（实测会得到空树）。 */
			while (nodes.size() == 1 && nodes[0].IsFolder())
			{
				std::vector<ShaderMenuNode> inner = std::move(nodes[0].Children);
				nodes = std::move(inner);
			}

			/* 当前 Shader 不在清单里（资产外路径之类）：树顶补一条，选中态照常能看到 */
			if (!current.empty()
				&& std::find(options_cache.begin(), options_cache.end(), current) == options_cache.end())
				nodes.insert(nodes.begin(), ShaderMenuNode{ current, current, {} });

			DrawShaderMenuLevel(nodes, current, picked);
			ImGui::EndPopup();
		}

		ImGuiExt::EndPropertyRow();
		return (picked != current) ? picked : std::string();
	}

	/* 直接改材质的 Shader：资产编辑缓冲这类"就地写"的场合用 */
	inline bool DrawMaterialShaderRow(Material& material, std::vector<std::string>& options_cache)
	{
		const std::string picked = DrawMaterialShaderPicker(material, options_cache);
		if (picked.empty())
			return false;

		material.SetShader(AssetManager::Instance().GetOrLoad<DeviceShader>(ABSOLUTE_PATH(picked)));
		return true;
	}

	/* 一条材质参数行：值画在临时缓冲上，改动经 write_back 落进目标材质（槽位 → 自动实例化
	 * 写回；资产编辑缓冲 / 模型编辑器 → 直接写）。行对齐：标签跟 frame 高控件垂直居中。
	 * 贴图行只画缩略图（点击定位、拖入替换、悬停看大图）。
	 * spec 决定标签文案与控件形态（@property 显示名 / @type / @range）。 */
	inline void DrawMaterialParamRow(const MaterialParamInfo& param_info, const MaterialPropertySpec& spec,
		const std::function<void(const std::string&)>& reveal_asset,
		const std::function<void(ParamType, const std::string&, const std::any&)>& write_back)
	{
		bool edited = false;
		const std::string& param_name = param_info.Name;
		const char* label = spec.Label.empty() ? param_name.c_str() : spec.Label.c_str();
		const float kLabelWidth = EditorTheme::Token::PropertyLabelWidth;

		/* 显示名可以重复、参数名不会：行 ID 按参数名压栈，换标签不影响控件身份 */
		ImGui::PushID(param_name.c_str());

		switch (param_info.Type)
		{
		case ParamType::Texture:
			{
				auto texture_info = std::any_cast<std::pair<SharedPtr<DeviceTexture>, uint32_t>>(param_info.Value);
				auto texture = texture_info.first; /* 临时副本 */

				ImGuiExt::BeginPropertyRow(label, kLabelWidth, true);

				const auto& show_texture = texture ? texture
					: AssetManager::Instance().GetOrLoad<DeviceTexture>(ABSOLUTE_PATH("Textures/Default.png"));

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
							const auto new_texture = AssetManager::Instance().GetOrLoad<DeviceTexture>(
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

				/* 点击缩略图 = 在资源浏览器里定位这张贴图（没接定位通道时点击无效果） */
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
				const float value_width = ImGuiExt::BeginPropertyRow(label, kLabelWidth, true);
				ImGui::SetNextItemWidth(value_width);
				if (spec.Widget == "bool")
				{
					bool checked = value != 0;
					edited = ImGui::Checkbox("##Value", &checked);
					value = checked ? 1 : 0;
				}
				else if (spec.Widget == "slider" && spec.HasRange)
				{
					edited = ImGui::SliderInt("##Value", &value,
						static_cast<int>(spec.Min), static_cast<int>(spec.Max));
				}
				else
				{
					edited = ImGui::DragInt("##Value", &value, spec.Step > 0.0f ? spec.Step : 0.1f,
						spec.HasRange ? static_cast<int>(spec.Min) : 0,
						spec.HasRange ? static_cast<int>(spec.Max) : 0);
				}
				ImGuiExt::EndPropertyRow();
				if (edited)
					write_back(ParamType::Int, param_name, value);
			}
			break;
		case ParamType::Float:
			{
				float value = std::any_cast<float>(param_info.Value);
				const float value_width = ImGuiExt::BeginPropertyRow(label, kLabelWidth, true);
				ImGui::SetNextItemWidth(value_width);
				if (spec.Widget == "slider" && spec.HasRange)
					edited = ImGui::SliderFloat("##Value", &value, spec.Min, spec.Max);
				else
					edited = ImGui::DragFloat("##Value", &value, spec.Step > 0.0f ? spec.Step : 0.01f,
						spec.HasRange ? spec.Min : 0.0f, spec.HasRange ? spec.Max : 0.0f);
				ImGuiExt::EndPropertyRow();
				if (edited)
					write_back(ParamType::Float, param_name, value);
			}
			break;
		case ParamType::Vec2:
			{
				glm::vec2 value = std::any_cast<glm::vec2>(param_info.Value);
				const float value_width = ImGuiExt::BeginPropertyRow(label, kLabelWidth, true);
				ImGui::SetNextItemWidth(value_width);
				edited = ImGui::DragFloat2("##Value", glm::value_ptr(value), spec.Step > 0.0f ? spec.Step : 0.01f,
					spec.HasRange ? spec.Min : 0.0f, spec.HasRange ? spec.Max : 0.0f);
				ImGuiExt::EndPropertyRow();
				if (edited)
					write_back(ParamType::Vec2, param_name, value);
			}
			break;
		case ParamType::Vec3:
			{
				glm::vec3 value = std::any_cast<glm::vec3>(param_info.Value);
				const float value_width = ImGuiExt::BeginPropertyRow(label, kLabelWidth, true);
				ImGui::SetNextItemWidth(value_width);
				if (spec.Widget == "color")
					edited = ImGui::ColorEdit3("##Value", glm::value_ptr(value));
				else
					edited = ImGui::DragFloat3("##Value", glm::value_ptr(value), spec.Step > 0.0f ? spec.Step : 0.01f,
						spec.HasRange ? spec.Min : 0.0f, spec.HasRange ? spec.Max : 0.0f);
				ImGuiExt::EndPropertyRow();
				if (edited)
					write_back(ParamType::Vec3, param_name, value);
			}
			break;
		case ParamType::Vec4:
			{
				glm::vec4 value = std::any_cast<glm::vec4>(param_info.Value);
				const float value_width = ImGuiExt::BeginPropertyRow(label, kLabelWidth, true);
				ImGui::SetNextItemWidth(value_width);
				if (spec.Widget == "color")
					edited = ImGui::ColorEdit4("##Value", glm::value_ptr(value));
				else
					edited = ImGui::DragFloat4("##Value", glm::value_ptr(value), spec.Step > 0.0f ? spec.Step : 0.01f,
						spec.HasRange ? spec.Min : 0.0f, spec.HasRange ? spec.Max : 0.0f);
				ImGuiExt::EndPropertyRow();
				if (edited)
					write_back(ParamType::Vec4, param_name, value);
			}
			break;
		default:
			/* Mat4 等没做编辑器，跳过（只读展示也省） */
			break;
		}

		ImGui::PopID();
	}
}
