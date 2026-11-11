#include "Pch.h"
#include "ShaderVariantPanel.h"

#include <algorithm>
#include <cstdio>
#include <map>

#include <imgui.h>

#include "Common/TextMatch.h"
#include "Panels/PanelWindow.h"
#include "EditorUI/EditorIcons.h"
#include "EditorUI/EditorTheme.h"
#include "EditorUI/ImGuiExtensions.h"
#include "EditorUI/PanelChrome.h"
#include "EditorUI/PanelRegistry.h"
#include "Helios/Core/AssetManager.h"
#include "Helios/Renderer/ShaderVariantStreamer.h"
#include "Helios/VirtualDevice/DeviceShader.h"

namespace Helios
{
	namespace
	{
		/* 路径只显示文件名与上一级目录：Assets/Shaders/MaterialShaders/PBRStandard.glsl
		 * 只留 MaterialShaders/PBRStandard.glsl，面板宽度有限 */
		std::string ShortPath(const std::string& path)
		{
			if (path.empty())
				return "(embedded)";

			const size_t last = path.find_last_of("/\\");
			if (last == std::string::npos)
				return path;

			const size_t prev = path.find_last_of("/\\", last - 1);
			return (prev == std::string::npos) ? path.substr(last + 1) : path.substr(prev + 1);
		}

		/* 变体表：define 列吃满剩余宽度，耗时 / 代号 / 哈希按内容定宽 —— 列宽不随数值抖动 */
		void DrawVariantTable(const std::vector<ShaderVariantPanel::VariantRow>& variants)
		{
			const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
				| ImGuiTableFlags_NoBordersInBody | ImGuiTableFlags_SizingFixedFit;

			if (!ImGui::BeginTable("##variants", 5, flags))
				return;

			const float number_width = ImGui::CalcTextSize("000.0 ms").x;
			ImGui::TableSetupColumn("Defines", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Convert", ImGuiTableColumnFlags_WidthFixed, number_width);
			ImGui::TableSetupColumn("Build", ImGuiTableColumnFlags_WidthFixed, number_width);
			ImGui::TableSetupColumn("Gen", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("000").x);
			ImGui::TableSetupColumn("Hash", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("00000000").x);
			ImGui::TableHeadersRow();

			for (const ShaderVariantPanel::VariantRow& row : variants)
			{
				ImGui::TableNextRow();

				ImGui::TableNextColumn();
				/* define 串可能长过列宽（窄面板）：裁成省略号，悬停看全文 */
				const float cs_width = row.IsCompute
					? ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize("CS").x : 0.0f;
				const float defines_width = ImMax(ImGui::GetContentRegionAvail().x - cs_width, 1.0f);
				const bool clipped = ImGui::CalcTextSize(row.Defines.c_str()).x > defines_width;

				if (row.Defines == "(default)")
				{
					ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
					PanelChrome::DrawClippedTextLine(row.Defines.c_str(), defines_width);
					ImGui::PopStyleColor();
				}
				else
				{
					PanelChrome::DrawClippedTextLine(row.Defines.c_str(), defines_width);
				}

				if (clipped && ImGui::IsItemHovered())
					ImGui::SetTooltip("%s", row.Defines.c_str());

				if (row.IsCompute)
				{
					ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
					ImGui::TextDisabled("CS");
				}

				ImGui::TableNextColumn();
				ImGui::Text("%.1f ms", row.ConvertMs);

				ImGui::TableNextColumn();
				/* 驱动编译要等第一次绘制，没到过就是 0，别显示成"很快" */
				if (row.DeviceBuildMs > 0.0)
				{
					ImGui::Text("%.1f ms", row.DeviceBuildMs);
				}
				else
				{
					ImGui::TextDisabled("--");
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("The driver compiles this variant on first use");
				}

				ImGui::TableNextColumn();
				/* 代号大于 1 说明热重载后重编过，正文色提出来；首版压暗不抢注意力 */
				if (row.Generation > 1)
					ImGui::Text("%u", row.Generation);
				else
					ImGui::TextDisabled("%u", row.Generation);

				ImGui::TableNextColumn();
				ImGui::TextDisabled("%s", row.Hash.c_str());
			}

			ImGui::EndTable();
		}
	}

	std::string ShaderVariantPanel::FormatDefines(const ShaderVariantDefines& defines)
	{
		std::string text;
		for (const auto& [name, value] : defines)
		{
			if (!text.empty())
				text += ' ';
			text += name;
			text += '=';
			text += std::to_string(value);
		}
		return text;
	}

	void ShaderVariantPanel::Collect(std::vector<ShaderGroup>& groups)
	{
		groups.clear();

		/* 同一份源码的多个变体按路径归到一组；内嵌源码的实例没有路径，不列 */
		std::map<std::string, std::vector<SharedPtr<DeviceShader>>> by_path;
		for (const SharedPtr<DeviceShader>& shader : AssetManager::Instance().GetAll<DeviceShader>())
		{
			if (shader == nullptr || shader->GetPath().empty())
				continue;
			by_path[shader->GetPath()].push_back(shader);
		}

		groups.reserve(by_path.size());
		for (auto& [path, variants] : by_path)
		{
			std::sort(variants.begin(), variants.end(),
				[](const SharedPtr<DeviceShader>& a, const SharedPtr<DeviceShader>& b)
				{
					const ShaderVariantDefines& left = a->GetVariantDefines();
					const ShaderVariantDefines& right = b->GetVariantDefines();
					return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
						[](const auto& lhs, const auto& rhs)
						{
							return lhs.first != rhs.first ? lhs.first < rhs.first : lhs.second < rhs.second;
						});
				});

			ShaderGroup group;
			group.Path = path;

			/* 声明维度取任意一个变体的反射：变体不裁参数，声明在每份里都一样 */
			for (const ShaderVariantSpec& spec : variants.front()->GetReflectionData().VariantSpecs)
			{
				if (!group.DeclaredDims.empty())
					group.DeclaredDims += ", ";

				group.DeclaredDims += spec.Name;
				switch (spec.Source)
				{
				case ShaderVariantSource::View:
					group.DeclaredDims += " (view)";
					break;
				case ShaderVariantSource::Global:
					group.DeclaredDims += " (global)";
					break;
				case ShaderVariantSource::Draw:
					group.DeclaredDims += " (draw)";
					break;
				case ShaderVariantSource::Material:
					group.DeclaredDims += " (" + spec.ParamName + ")";
					break;
				}
			}

			for (const SharedPtr<DeviceShader>& shader : variants)
			{
				VariantRow row;
				row.Defines = FormatDefines(shader->GetVariantDefines());
				if (row.Defines.empty())
					row.Defines = "(default)";

				const std::string& hash = shader->GetSourceHash();
				row.Hash = hash.size() > 8 ? hash.substr(0, 8) : hash;
				row.ConvertMs = shader->GetLoadTimeMs();
				row.DeviceBuildMs = shader->GetDeviceBuildMs();
				row.TotalMs = row.ConvertMs + row.DeviceBuildMs;
				row.Generation = shader->GetGeneration();
				row.IsCompute = shader->IsCompute();
				group.TotalMs += row.TotalMs;
				group.Variants.push_back(std::move(row));
			}

			groups.push_back(std::move(group));
		}
	}

	size_t ShaderVariantPanel::ApplyFilter(std::vector<ShaderGroup>& groups, const FilterOptions& options,
		const std::string& lowercase_needle, bool sort_by_time)
	{
		size_t kept_variants = 0;

		for (ShaderGroup& group : groups)
		{
			/* 变体数按裁剪前判：搜到一个变体不代表这份着色器只编过一份 */
			const bool multi_variant = group.Variants.size() > 1;
			if (options.MultiVariantOnly && !multi_variant)
			{
				group.Variants.clear();
				continue;
			}

			/* 路径与声明维度命中就留整组；否则逐行比 define 与哈希 */
			const bool group_hit = ContainsCaseInsensitive(group.Path, lowercase_needle)
				|| ContainsCaseInsensitive(group.DeclaredDims, lowercase_needle);

			std::vector<VariantRow> kept;
			kept.reserve(group.Variants.size());
			for (VariantRow& row : group.Variants)
			{
				if (!group_hit && !ContainsCaseInsensitive(row.Defines, lowercase_needle)
					&& !ContainsCaseInsensitive(row.Hash, lowercase_needle))
					continue;
				if (options.ComputeOnly && !row.IsCompute)
					continue;
				if (options.DeviceBuiltOnly && row.DeviceBuildMs <= 0.0)
					continue;
				kept.push_back(std::move(row));
			}

			group.Variants = std::move(kept);
			if (group.Variants.empty())
				continue;

			/* 总耗时按留下的行重算：筛选后组序要跟着看得到的数字走 */
			group.TotalMs = 0.0;
			for (const VariantRow& row : group.Variants)
				group.TotalMs += row.TotalMs;

			if (sort_by_time)
			{
				std::sort(group.Variants.begin(), group.Variants.end(),
					[](const VariantRow& a, const VariantRow& b) { return a.TotalMs > b.TotalMs; });
			}

			kept_variants += group.Variants.size();
		}

		groups.erase(std::remove_if(groups.begin(), groups.end(),
			[](const ShaderGroup& group) { return group.Variants.empty(); }), groups.end());

		if (sort_by_time)
		{
			std::sort(groups.begin(), groups.end(),
				[](const ShaderGroup& a, const ShaderGroup& b) { return a.TotalMs > b.TotalMs; });
		}

		return kept_variants;
	}

	bool ShaderVariantPanel::HasActiveFilters() const
	{
		return m_Filters.MultiVariantOnly || m_Filters.ComputeOnly || m_Filters.DeviceBuiltOnly;
	}

	/* 工具行：搜索框吃左端余量，行尾两枚图标按钮（清除 / 筛选）。
	 * 宽度账与统计面板的图表工具条同一套：按钮先占住行尾，搜索框拿剩下的。 */
	void ShaderVariantPanel::ShowToolbar()
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const float row_width = ImGui::GetContentRegionAvail().x;
		const float button = ImGui::GetFrameHeight();
		const bool searching = m_Search[0] != '\0';

		const float filter_x = ImMax(row_width - button, 0.0f);
		const float clear_x = filter_x - style.ItemSpacing.x - button;
		const float search_right = searching ? clear_x : filter_x;
		const float search_width = ImMax(search_right - style.ItemSpacing.x, 80.0f);

		Icons::BeginSearchInput();
		ImGui::SetNextItemWidth(search_width);
		if (ImGui::InputTextWithHint("##VariantSearch", "Search...", m_Search, sizeof(m_Search)))
			m_Filter = ToLowercase(m_Search);
		Icons::EndSearchInput();

		if (searching)
		{
			ImGuiExt::SameLineAt(clear_x);
			if (Icons::IconButton(Icons::Id::Remove, ImVec2(button, button), false, "Clear search"))
			{
				m_Search[0] = '\0';
				m_Filter.clear();
			}
		}

		/* 筛选钮常亮表示"有条件在生效"，弹层里同时管排序。
		 * 弹层开着时再点不再重开（否则先关后开，闪一下） */
		const bool filtering = HasActiveFilters();
		const bool popup_open = ImGui::IsPopupOpen("##VariantFilters");
		ImGuiExt::SameLineAt(filter_x);
		if (Icons::IconButton(Icons::Id::Filter, ImVec2(button, button), filtering,
			filtering ? "Filters active" : "Filters and sorting") && !popup_open)
		{
			/* 锚点只在打开这一刻取：弹层贴按钮下方、右缘对齐 */
			const ImVec2 anchor = ImGui::GetItemRectMax();
			m_FilterMenuAnchorX = anchor.x;
			m_FilterMenuAnchorY = anchor.y;
			ImGui::OpenPopup("##VariantFilters");
		}

		ShowFilterPopup();
	}

	void ShaderVariantPanel::ShowFilterPopup()
	{
		if (!ImGui::IsPopupOpen("##VariantFilters"))
			return;

		ImGui::SetNextWindowPos(ImVec2(m_FilterMenuAnchorX, m_FilterMenuAnchorY + 4.0f),
			ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));

		/* 最小宽度取最长行：勾选列对齐后弹层不会被撑成一条一条 */
		const ImGuiStyle& style = ImGui::GetStyle();
		const float min_width = ImGui::CalcTextSize("Multi-variant only").x
			+ style.FramePadding.x * 2.0f + style.ItemInnerSpacing.x * 2.0f
			+ ImGui::GetFontSize() * 2.0f + 24.0f;
		ImGui::SetNextWindowSizeConstraints(ImVec2(min_width, 0.0f), ImVec2(FLT_MAX, FLT_MAX));

		/* 弹层圆角与内边距得压在 Begin 之前；工具栏弹层行贴行（纵向行距归零） */
		PanelChrome::PushMenuStyle(true);
		if (ImGui::BeginPopup("##VariantFilters"))
		{
			PanelChrome::MenuSectionHeader("SORT");
			if (PanelChrome::MenuItemSelectWithIcon(Icons::Id::Menu, "Shader name", !m_SortByTime))
				m_SortByTime = false;
			if (PanelChrome::MenuItemSelectWithIcon(Icons::Id::Stats, "Total cost", m_SortByTime))
				m_SortByTime = true;

			ImGui::Separator();

			PanelChrome::MenuSectionHeader("SHOW");
			PanelChrome::MenuItemToggleWithIcon(Icons::Id::None, "Multi-variant only",
				&m_Filters.MultiVariantOnly);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Only shaders with more than one loaded variant");

			PanelChrome::MenuItemToggleWithIcon(Icons::Id::None, "Compute only", &m_Filters.ComputeOnly);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Only compute variants");

			PanelChrome::MenuItemToggleWithIcon(Icons::Id::None, "Device built only",
				&m_Filters.DeviceBuiltOnly);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Only variants the driver has already compiled. Their cost is real");

			ImGui::EndPopup();
		}
		PanelChrome::PopMenuStyle();
	}

	/* 汇总行：总量 / 命中量 / 后台编译进度。计数都是本帧的真实值 */
	void ShaderVariantPanel::ShowSummary(size_t shown_shaders, size_t total_shaders,
		size_t shown_variants, size_t total_variants, size_t multi_variant)
	{
		char text[192];
		if (shown_shaders == total_shaders && shown_variants == total_variants)
		{
			std::snprintf(text, sizeof(text), "%zu shaders \xc2\xb7 %zu variants \xc2\xb7 %zu multi-variant",
				total_shaders, total_variants, multi_variant);
		}
		else
		{
			std::snprintf(text, sizeof(text), "%zu of %zu shaders \xc2\xb7 %zu of %zu variants",
				shown_shaders, total_shaders, shown_variants, total_variants);
		}

		/* 变体在后台编，面板只报进度：卡顿看这里，命中率看变体数 */
		const size_t pending = ShaderVariantStreamer::Instance().Pending();
		if (pending == 0)
		{
			ImGui::TextDisabled("%s", text);
			return;
		}

		ImGui::TextDisabled("%s \xc2\xb7 %zu compiling", text, pending);
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%zu queued, %u finished this session", pending,
				ShaderVariantStreamer::Instance().CompletedGeneration());
		}
	}

	/* 一个着色器一张卡：卡头是短路径 + 变体数徽章，卡身是完整路径、维度声明与变体表 */
	void ShaderVariantPanel::ShowGroupCard(const ShaderGroup& group, bool force_open)
	{
		/* 短路径可能撞名（不同根下的同名目录），身份带上完整路径 */
		ImGui::PushID(group.Path.c_str());

		const PanelChrome::Card card = PanelChrome::BeginCard(ShortPath(group.Path).c_str(),
			Icons::Id::FileShader, Icons::Id::None, nullptr, true, nullptr, force_open);

		char badge[32];
		std::snprintf(badge, sizeof(badge), "%zu variant%s", group.Variants.size(),
			group.Variants.size() == 1 ? "" : "s");
		PanelChrome::DrawCardBadge(card, badge, group.Path.c_str());

		if (card.Open)
		{
			const float width = ImGui::GetContentRegionAvail().x;

			ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
			PanelChrome::DrawClippedTextLine(group.Path.c_str(), width);
			ImGui::PopStyleColor();

			std::string meta;
			if (group.TotalMs > 0.0)
			{
				char buffer[48];
				std::snprintf(buffer, sizeof(buffer), "%.1f ms total", group.TotalMs);
				meta = buffer;
			}
			if (!group.DeclaredDims.empty())
			{
				if (!meta.empty())
					meta += "  \xc2\xb7  ";
				meta += "declared: ";
				meta += group.DeclaredDims;
			}

			if (!meta.empty())
			{
				ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
				ImGui::TextWrapped("%s", meta.c_str());
				ImGui::PopStyleColor();
			}

			DrawVariantTable(group.Variants);
		}

		PanelChrome::EndCard(card);
		ImGui::PopID();
	}

	void ShaderVariantPanel::OnImGuiRender()
	{
		bool& visible = Panel::VisibleFlag(Panel::kShaderVariants);
		PanelWindow window(Panel::kShaderVariants, &visible);
		if (!window)
			return;

		std::vector<ShaderGroup> groups;
		Collect(groups);

		size_t total_variants = 0;
		size_t multi_variant = 0;
		for (const ShaderGroup& group : groups)
		{
			total_variants += group.Variants.size();
			if (group.Variants.size() > 1)
				++multi_variant;
		}
		const size_t total_shaders = groups.size();

		ShowToolbar();

		/* 搜索词 / 筛选 / 排序都在这一趟落定，后面只画结果 */
		const bool querying = !m_Filter.empty() || HasActiveFilters();
		const size_t shown_variants = ApplyFilter(groups, m_Filters, m_Filter, m_SortByTime);

		ShowSummary(groups.size(), total_shaders, shown_variants, total_variants, multi_variant);
		ImGui::Separator();

		if (groups.empty())
		{
			ImGui::TextDisabled("%s", total_shaders == 0 ? "No shaders loaded"
				: "Nothing matches the current search or filters");
			return;
		}

		/* 过滤中强制展开：命中的变体不能被折叠挡住 */
		for (const ShaderGroup& group : groups)
			ShowGroupCard(group, querying);
	}
}
