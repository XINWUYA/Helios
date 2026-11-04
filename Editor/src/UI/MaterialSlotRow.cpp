#include "Pch.h"
#include "MaterialSlotRow.h"
#include "EditorTheme.h"
#include "ImGuiExtensions.h"
#include "Helios/Common/PathUtils.h"
#include "Helios/Core/AssetManager.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace Helios::MaterialSlotRow
{
	namespace
	{
		/* 资源引用输入框：只读展示资源名（文本可选中复制），拖放目标与 tooltip 由调用方挂上 */
		void DrawRefInputBox(const char* id, const std::string& text, float width, const char* hint)
		{
			char buffer[256] = {};
			std::strncpy(buffer, text.c_str(), sizeof(buffer) - 1);

			ImGui::SetNextItemWidth(width);
			ImGui::InputTextWithHint(id, hint, buffer, sizeof(buffer), ImGuiInputTextFlags_ReadOnly);
		}
	}

	Result Draw(const State& state)
	{
		Result result;

		const std::string display = state.Path.empty()
			? state.EmptyText
			: PathToUtf8(PathFromUtf8(state.Path).filename());

		const float gap = ImGui::GetStyle().ItemInnerSpacing.x;

		const float value_width = ImGuiExt::BeginPropertyRow(state.Label);

		/* 行内自算尺寸（按钮宽高）要在 Begin 之后取：行高由 BeginPropertyRow 统一压成字体高 */
		const float frame_height = ImGui::GetFrameHeight();

		const bool can_reset = state.IsInstance && !state.Path.empty();
		const float reset_width = can_reset
			? ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2.0f
			: 0.0f;
		float button_zone = state.CanClear ? (frame_height + gap) : 0.0f;
		if (can_reset)
			button_zone += reset_width + gap;

		/* 实例标记放值列开头：一眼分出哪一行是改出来的副本，占的宽度从文件名里扣 */
		float chip_width = 0.0f;
		const float instance_width = ImGui::CalcTextSize("Instance").x + ImGui::GetStyle().FramePadding.x * 2.0f + gap;
		if (state.IsInstance && value_width - button_zone - instance_width >= frame_height * 2.0f)
		{
			chip_width = ImGuiExt::DrawTagChip("Instance", frame_height) + gap;
			ImGui::SameLine(0.0f, gap);
		}

		const float target_width = std::max(1.0f, value_width - button_zone - chip_width);

		DrawRefInputBox("##MaterialRef", display, target_width, state.EmptyHint);

		if (ImGui::IsItemHovered())
		{
			/* 框里只放文件名，完整路径挂在悬停里；实例行额外说明改的值不在来源文件里 */
			std::string tooltip;
			if (!state.HoverPath.empty())
			{
				tooltip = state.HoverPath;
				tooltip += "\n\n";
			}
			tooltip += state.EmptyHint;
			if (state.IsInstance)
				tooltip += "\n\nSlot instance — edits stay on this entity, Reset re-clones the asset";
			ImGui::SetTooltip("%s", tooltip.c_str());
		}

		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RESOURCE_BROWSER_ITEM"))
			{
				std::filesystem::path relative_path;
				if (payload->DataSize > 1 && TryPathFromUtf8Payload(
					payload->Data, static_cast<size_t>(payload->DataSize), relative_path))
				{
					if (relative_path.extension() == ".mtl")
					{
						const std::string absolute_path = PathToUtf8(ResolveAssetPath(relative_path));
						auto material = AssetManager::Instance().GetOrLoad<Material>(absolute_path);
						if (material != nullptr)
						{
							result.Act = Action::Assign;
							result.Picked = std::move(material);
							result.SourcePath = absolute_path;
						}
					}
				}
			}
			ImGui::EndDragDropTarget();
		}

		if (can_reset)
		{
			ImGui::SameLine(0.0f, gap);
			if (ImGui::Button("Reset", ImVec2(reset_width, frame_height)))
				result.Act = Action::Reset;

			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", state.ResetHint);
		}

		if (state.CanClear)
		{
			ImGui::SameLine(0.0f, gap);
			if (ImGui::Button("X", ImVec2(frame_height, frame_height)))
				result.Act = Action::Clear;

			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", state.ClearHint);
		}

		ImGuiExt::EndPropertyRow();
		return result;
	}
}
