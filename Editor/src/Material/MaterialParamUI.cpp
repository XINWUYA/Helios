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
#include "UI/BuiltinTextures.h"
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

		if (const auto hit = s_Cache.find(shader.get()); hit != s_Cache.end()
			&& hit->second.Shader.lock() == shader && hit->second.Generation == shader->GetGeneration())
			return hit->second.Entries;

		/* 要重建时顺手清掉已经销毁的 Shader 留下的条目，缓存不会越攒越多 */
		for (auto iter = s_Cache.begin(); iter != s_Cache.end();)
			iter = iter->second.Shader.expired() ? s_Cache.erase(iter) : std::next(iter);

		CacheEntry& cached = s_Cache[shader.get()];
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

	bool BeginPropertyGroup(const char* id, const char* title, bool default_open, const char* suffix)
	{
		ImGui::PushID(id);
		ImGui::SetNextItemOpen(default_open, ImGuiCond_Once);
		ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::Clear);
		const bool open = ImGui::TreeNodeEx("##Group", ImGuiTreeNodeFlags_SpanFullWidth
			| ImGuiTreeNodeFlags_NoTreePushOnOpen, "%s", "");
		ImGui::PopStyleColor();
		const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
		const float arrow = ImGui::GetFontSize() * 0.55f;
		const float y = (min.y + max.y) * 0.5f;
		const float text_y = y - ImGui::GetFontSize() * 0.5f;
		const float text_x = min.x + arrow + ImGui::GetStyle().ItemInnerSpacing.x;
		ImDrawList* draw = ImGui::GetWindowDrawList();
		PanelChrome::DrawDisclosureArrow(draw, ImVec2(min.x + arrow * 0.5f, y), arrow, open,
			ImGui::GetColorU32(EditorTheme::Token::TextDim));
		draw->PushClipRect(min, max, true);
		/* 小标题按卡片惯例全大写；中文没有小写字母，原样保留 */
		std::string display_title(title);
		for (char& ch : display_title)
		{
			if (ch >= 'a' && ch <= 'z')
				ch = static_cast<char>(ch - 'a' + 'A');
		}
		/* 小标题用面板标题字体（SemiBold）按正文字号画：比参数行重一档，又不抢卡片头的级 */
		const ImFont* title_font = (EditorTheme::GetFonts().Title != nullptr)
			? EditorTheme::GetFonts().Title : ImGui::GetFont();
		draw->AddText(title_font, ImGui::GetFontSize(), ImVec2(text_x, text_y),
			ImGui::GetColorU32(EditorTheme::Token::TextLabel), display_title.c_str());
		if (suffix != nullptr && *suffix != '\0')
		{
			const float title_width = title_font->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0f,
				display_title.c_str()).x;
			draw->AddText(ImVec2(text_x + title_width + ImGui::GetStyle().ItemInnerSpacing.x, text_y),
				ImGui::GetColorU32(EditorTheme::Token::TextDim), suffix);
		}
		draw->PopClipRect();
		ImGui::PopID();
		return open;
	}

	void DrawMaterialParams(const Material& material, const std::function<const Material*()>& resolve_live,
		const std::function<void(const std::string&)>& reveal_asset, const ParamWriter& write_back)
	{
		const auto rows = BuildMaterialParamRows(material);
		if (rows.empty())
			DrawMaterialReflectionNotice(material);
		std::vector<std::string> groups{ "" };
		for (const auto& row : rows)
			if (std::find(groups.begin(), groups.end(), row.Spec.Group) == groups.end())
				groups.push_back(row.Spec.Group);
		for (const std::string& group : groups)
		{
			if (!group.empty() && !BeginPropertyGroup(group.c_str(), group.c_str()))
				continue;
			ImGui::PushID(group.c_str());
			for (const auto& row : rows)
			{
				if (row.Spec.Group != group)
					continue;
				const Material* live = resolve_live();
				if (live == nullptr)
					break;
				const auto& parameters = live->GetAllParameters();
				const auto it = parameters.find(ToID(row.Param.Name));
				DrawMaterialParamRow(it != parameters.end() ? it->second : row.Param, row.Spec, reveal_asset, write_back);
			}
			ImGui::PopID();
		}
	}

	void DrawMaterialReflectionNotice(const Material& material)
	{
		const SharedPtr<DeviceShader> shader = material.GetShader();
		/* 扫过源码、只是没有裸 uniform（参数全在 uniform 块里）时不算故障 */
		const bool scanned = shader != nullptr && shader->GetReflectionData().MaterialParamsScanned;

		const char* message = nullptr;
		if (shader == nullptr)
			message = "No shader loaded — cannot reflect parameters";
		else if (!scanned)
			message = "Shader reflection unavailable — parameters not listed";
		else
			message = "No @property params — nothing to edit in this shader";

		/* 没东西可改是常态（天空这类吃 uniform 块的着色器就是这样），弱化即可；反射不到才报警色 */
		ImGui::PushStyleColor(ImGuiCol_Text,
			scanned ? EditorTheme::Token::TextDim : EditorTheme::Token::Danger);
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

	/* 当前启用的变体维度：声明了哪些维度、各自当前取值、值从哪儿来。
	 * 绘制级维度由绘制端逐笔决定，材质侧按 0 显示并标注 */
	void DrawMaterialVariantRow(const Material& material)
	{
		const SharedPtr<DeviceShader>& shader = material.GetShader();
		if (shader == nullptr)
			return;

		const auto& specs = shader->GetReflectionData().VariantSpecs;
		if (specs.empty())
			return;

		const ShaderVariantDefines defines = material.GetMaterialVariantDefines();

		std::string text;
		for (const ShaderVariantSpec& spec : specs)
		{
			if (!text.empty())
				text += "   ";

			const auto it = defines.find(spec.Name);
			text += spec.Name;
			text += '=';
			text += std::to_string(it == defines.end() ? 0 : it->second);

			switch (spec.Source)
			{
			case ShaderVariantSource::Material:
				break;
			case ShaderVariantSource::Draw:
				text += " (draw)";
				break;
			case ShaderVariantSource::View:
				text += " (view)";
				break;
			case ShaderVariantSource::Global:
				text += " (global)";
				break;
			}
		}

		ImGuiExt::BeginPropertyRow("Variants", EditorTheme::Token::PropertyLabelWidth, true);
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::TextWrapped("%s", text.c_str());
		ImGui::PopStyleColor();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Keyword values this material drives. Dimensions tagged (draw) are chosen by the pass.");
		ImGuiExt::EndPropertyRow();
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
				/* 缩略图显示的那张图的相对路径：内置图在资源浏览器里没有落点 */
				const std::string shown_relative = show_texture != nullptr
					? RELATIVE_PATH(show_texture->GetPath()) : std::string();
				const bool shown_builtin = BuiltinTextures::Contains(shown_relative);

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
					ImGui::TextDisabled("%s", shown_builtin
						? "Built-in texture" : "Click to reveal in the resource browser");
					ImGui::EndTooltip();
				}

				/* 内置贴图不在资源浏览器里，从缩略图右邻的下拉选 */
				ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
				std::string picked_builtin;
				if (BuiltinTextures::DrawPicker("##BuiltinTexture",
					texture != nullptr ? RELATIVE_PATH(texture->GetPath()) : std::string(), picked_builtin))
				{
					const auto picked_texture = AssetManager::Instance().GetOrLoad<DeviceTexture>(
						ABSOLUTE_PATH(picked_builtin));
					if (picked_texture->IsLoaded())
					{
						texture = picked_texture;
						edited = true;
					}
				}

				/* 点击缩略图 = 在资源浏览器里定位这张贴图（没接定位通道时点击无效果） */
				if (clicked && reveal_asset && !shown_builtin
					&& !shown_relative.empty() && shown_relative.rfind("..", 0) != 0)
					reveal_asset(shown_relative);

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
					edited = ImGuiExt::DrawSliderInt("##Value", value,
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
					edited = ImGuiExt::DrawSliderFloat("##Value", value, spec.Min, spec.Max, "%.3f");
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
					edited = ImGui::ColorEdit3("##Value", glm::value_ptr(value), ImGuiColorEditFlags_Float
						| (value_width < ImGui::GetFontSize() * 9.0f ? ImGuiColorEditFlags_NoInputs : 0));
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
						ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaBar
						| (value_width < ImGui::GetFontSize() * 11.0f ? ImGuiColorEditFlags_NoInputs : 0));
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
