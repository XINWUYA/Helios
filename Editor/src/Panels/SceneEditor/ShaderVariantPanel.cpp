#include "Pch.h"
#include "ShaderVariantPanel.h"

#include <algorithm>
#include <map>

#include <imgui.h>

#include "Panels/PanelWindow.h"
#include "UI/PanelRegistry.h"
#include "Helios/Core/AssetManager.h"
#include "Helios/Renderer/ShaderVariantCatalog.h"
#include "Helios/Renderer/ShaderVariants.h"
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
				case ShaderVariantSource::External:
					group.DeclaredDims += " (external)";
					break;
				case ShaderVariantSource::Param:
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
				row.Generation = shader->GetGeneration();
				row.IsCompute = shader->IsCompute();
				group.Variants.push_back(std::move(row));
			}

			groups.push_back(std::move(group));
		}
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

		ImGui::TextDisabled("%zu shaders, %zu variants, %zu with multiple variants",
			groups.size(), total_variants, multi_variant);

		/* 精确变体缺失时的两种处理：现编（默认）或用评分挑最接近的顶上 */
		bool fallback = ShaderVariantGlobals::FallbackEnabled();
		if (ImGui::Checkbox("Fall back to nearest variant", &fallback))
			ShaderVariantGlobals::SetFallbackEnabled(fallback);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Missing variants use the closest loaded one instead of compiling a new one");

		const size_t pending = ShaderVariantCatalog::Instance().Pending();
		if (pending > 0)
			ImGui::TextDisabled("Prewarming: %zu pending, %u done", pending,
				ShaderVariantCatalog::Instance().Completed());

		ImGui::Separator();

		if (groups.empty())
		{
			ImGui::TextDisabled("No shaders loaded");
			return;
		}

		for (const ShaderGroup& group : groups)
		{
			/* 节点标题：文件名 + 变体数；声明维度单独一行，太长时不挤标题 */
			const std::string label = ShortPath(group.Path) + "  (" + std::to_string(group.Variants.size()) + ")";
			const bool open = ImGui::TreeNodeEx(group.Path.c_str(),
				ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen, "%s", label.c_str());

			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s\n%s", group.Path.c_str(),
					group.DeclaredDims.empty() ? "no variant dimensions declared" : group.DeclaredDims.c_str());

			if (!open)
				continue;

			if (!group.DeclaredDims.empty())
				ImGui::TextDisabled("declared: %s", group.DeclaredDims.c_str());

			if (ImGui::BeginTable("##variants", 5,
				ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV))
			{
				ImGui::TableSetupColumn("Variant", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("Convert", ImGuiTableColumnFlags_WidthFixed, 70.0f);
				ImGui::TableSetupColumn("Build", ImGuiTableColumnFlags_WidthFixed, 70.0f);
				ImGui::TableSetupColumn("Gen", ImGuiTableColumnFlags_WidthFixed, 36.0f);
				ImGui::TableSetupColumn("Hash", ImGuiTableColumnFlags_WidthFixed, 78.0f);
				ImGui::TableHeadersRow();

				for (const VariantRow& row : group.Variants)
				{
					ImGui::TableNextRow();

					ImGui::TableNextColumn();
					if (row.IsCompute)
						ImGui::Text("%s  [CS]", row.Defines.c_str());
					else
						ImGui::TextUnformatted(row.Defines.c_str());

					ImGui::TableNextColumn();
					ImGui::Text("%.1f ms", row.ConvertMs);

					ImGui::TableNextColumn();
					/* 驱动编译要等第一次绘制，没到过就是 0，别显示成"很快" */
					if (row.DeviceBuildMs > 0.0)
						ImGui::Text("%.1f ms", row.DeviceBuildMs);
					else
						ImGui::TextDisabled("--");

					ImGui::TableNextColumn();
					ImGui::Text("%u", row.Generation);

					ImGui::TableNextColumn();
					ImGui::TextDisabled("%s", row.Hash.c_str());
				}

				ImGui::EndTable();
			}

			ImGui::TreePop();
		}
	}
}
