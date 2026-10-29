#include "Pch.h"
#include "MaterialParamUI.h"
#include <algorithm>
#include <cfloat>
#include <filesystem>
#include <unordered_map>
#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>
#include "Common/EditorCommon.h"
#include "UI/EditorIcons.h"
#include "UI/PanelChrome.h"
#include "Command/AssetFileOps.h"
#include "UI/EditorTheme.h"
#include "UI/ImGuiExtensions.h"
#include "Helios/Core/AssetManager.h"

namespace Helios::MaterialParamUI
{
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

	const std::vector<PropertyLayoutEntry>& PropertyLayoutOf(const SharedPtr<DeviceShader>& shader)
	{
		struct CacheEntry
		{
			WeakPtr<DeviceShader> Shader;
			uint32_t Generation{ 0 };
			std::vector<PropertyLayoutEntry> Entries;
		};
		static std::unordered_map<const DeviceShader*, CacheEntry> s_Cache;

		CacheEntry& cached = s_Cache[shader.get()];
		if (cached.Shader.lock() == shader && cached.Generation == shader->GetGeneration())
			return cached.Entries;

		cached.Shader = shader;
		cached.Generation = shader->GetGeneration();
		cached.Entries.clear();

		const ShaderReflectionData& reflection = shader->GetReflectionData();
		for (const ReflectedMaterialParam& reflected : reflection.MaterialParams)
		{
			/* 引擎自己绑的参数（如 u_ShadowMap / u_BRDFLut）没标 @property，不列：
			 * 贴图由渲染通道逐绘制给，面板改了也会被覆盖 */
			if (!reflected.Property)
				continue;

			PropertyLayoutEntry entry;
			entry.Reflected = &reflected;
			entry.ParamId = ToID(reflected.Name);
			entry.DefaultRow = MakeDefaultMaterialParamRow(reflected);
			if (!reflected.PropertySpec.ShowIfParam.empty())
			{
				entry.ShowIfId = ToID(reflected.PropertySpec.ShowIfParam);
				entry.ShowIfDeclared = reflection.FindMaterialParam(reflected.PropertySpec.ShowIfParam);
			}
			cached.Entries.push_back(std::move(entry));
		}

		std::sort(cached.Entries.begin(), cached.Entries.end(),
			[](const PropertyLayoutEntry& lhs, const PropertyLayoutEntry& rhs) { return lhs.Reflected->Name < rhs.Reflected->Name; });
		return cached.Entries;
	}

	bool IsLayoutEntryVisible(const Material& material, const PropertyLayoutEntry& entry)
	{
		const MaterialPropertySpec& spec = entry.Reflected->PropertySpec;
		if (spec.ShowIfParam.empty())
			return true;

		const auto& parameters = material.GetAllParameters();
		const std::any* value = nullptr;
		if (const auto iter = parameters.find(entry.ShowIfId); iter != parameters.end())
			value = &iter->second.Value;
		else if (entry.ShowIfDeclared != nullptr)
			value = &entry.ShowIfDeclared->Default;

		if (value == nullptr)
			return true;
		if (value->type() == typeid(int))
			return static_cast<float>(std::any_cast<int>(*value)) == spec.ShowIfValue;
		if (value->type() == typeid(float))
			return std::any_cast<float>(*value) == spec.ShowIfValue;
		return true;
	}

	std::vector<MaterialParamRow> BuildMaterialParamRows(const Material& material)
	{
		std::vector<MaterialParamRow> rows;

		const SharedPtr<DeviceShader> shader = material.GetShader();
		if (shader == nullptr)
			return rows;

		const std::vector<PropertyLayoutEntry>& layout = PropertyLayoutOf(shader);
		const auto& parameters = material.GetAllParameters();
		rows.reserve(layout.size());
		for (const PropertyLayoutEntry& entry : layout)
		{
			if (!IsLayoutEntryVisible(material, entry))
				continue;

			const auto iter = parameters.find(entry.ParamId);
			rows.push_back({ iter != parameters.end() ? iter->second : entry.DefaultRow, entry.Reflected->PropertySpec });
		}
		return rows;
	}

	void DrawMaterialReflectionNotice(const Material& material)
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

	std::string DrawMaterialShaderPicker(const Material& material)
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
			const std::vector<std::string>& options = CollectShaderOptions();
			std::vector<ShaderMenuNode> nodes = BuildShaderMenuTree(options);

			/* 清单都窝在同一条目录链里时（如 Shaders/MaterialShaders/…）逐层脱掉空壳，
			 * 顶层直接放内容。注意：先搬到局部再赋回 —— children 住在 nodes 自己的缓冲里，直接
			 * 自引用 move 行为未定义（实测会得到空树）。 */
			while (nodes.size() == 1 && nodes[0].IsFolder())
			{
				std::vector<ShaderMenuNode> inner = std::move(nodes[0].Children);
				nodes = std::move(inner);
			}

			/* 当前 Shader 不在清单里（资产外路径之类）：树顶补一条，选中态照常能看到 */
			if (!current.empty()
				&& std::find(options.begin(), options.end(), current) == options.end())
				nodes.insert(nodes.begin(), ShaderMenuNode{ current, current, {} });

			DrawShaderMenuLevel(nodes, current, picked);
			ImGui::EndPopup();
		}

		ImGuiExt::EndPropertyRow();
		return (picked != current) ? picked : std::string();
	}

	bool DrawMaterialShaderRow(Material& material)
	{
		const std::string picked = DrawMaterialShaderPicker(material);
		if (picked.empty())
			return false;

		material.SetShader(AssetManager::Instance().GetOrLoad<DeviceShader>(ABSOLUTE_PATH(picked)));
		return true;
	}

	bool DrawMaterialRenderQueueRow(int32_t& render_queue)
	{
		const float value_width = ImGuiExt::BeginPropertyRow("Render Queue", EditorTheme::Token::PropertyLabelWidth, true);
		int value = render_queue;
		ImGui::SetNextItemWidth(value_width);
		const bool edited = ImGui::DragInt("##RenderQueue", &value, 10.0f, 0, 5000);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Below 2500 draws opaque, 2500 and above draws transparent");
		ImGuiExt::EndPropertyRow();

		if (edited)
			render_queue = value;
		return edited;
	}

	void DrawMaterialParamRow(const MaterialParamInfo& param_info, const MaterialPropertySpec& spec,
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
								PathToUtf8(ResolveAssetPath(relative_path)));
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
					ImGui::TextDisabled("Click to reveal in the resource browser");
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
					edited = ImGui::ColorEdit3("##Value", glm::value_ptr(value), ImGuiColorEditFlags_Float);
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
					edited = ImGui::ColorEdit4("##Value", glm::value_ptr(value),
						ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaBar);
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
