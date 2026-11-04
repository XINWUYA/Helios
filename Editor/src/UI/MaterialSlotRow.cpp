#include "Pch.h"
#include "MaterialSlotRow.h"
#include "ImGuiExtensions.h"
#include "Helios/Common/PathUtils.h"
#include "Helios/Core/AssetManager.h"
#include <filesystem>

namespace Helios::MaterialSlotRow
{
	Result Draw(const State& state)
	{
		Result result;

		ImGuiExt::AssetRefRowOptions options;
		options.Label = state.Label;
		options.Text = state.Path.empty()
			? state.EmptyText
			: PathToUtf8(PathFromUtf8(state.Path).filename());
		options.Hint = state.EmptyHint;
		options.Chip = state.IsInstance ? "Instance" : nullptr;
		options.ShowReset = state.IsInstance && !state.Path.empty();
		options.ShowClear = state.CanClear;
		options.ResetHint = state.ResetHint;
		options.ClearHint = state.ClearHint;

		/* 框里只放文件名，完整路径挂在悬停里；实例行额外说明改的值不在来源文件里 */
		if (!state.HoverPath.empty())
			options.Tooltip = state.HoverPath + "\n\n";
		options.Tooltip += state.EmptyHint;
		if (state.IsInstance)
			options.Tooltip += "\n\nSlot instance — edits stay on this entity, Reset re-clones the asset";

		const ImGuiExt::AssetRefRowResult row = ImGuiExt::DrawAssetRefRow(options);
		if (row.Reset)
			result.Act = Action::Reset;
		else if (row.Clear)
			result.Act = Action::Clear;
		else if (!row.DroppedPath.empty())
		{
			const std::filesystem::path relative_path = PathFromUtf8(row.DroppedPath);
			if (relative_path.extension() == ".mtl")
			{
				const std::string absolute_path = PathToUtf8(ResolveAssetPath(relative_path));
				if (auto material = AssetManager::Instance().GetOrLoad<Material>(absolute_path))
				{
					result.Act = Action::Assign;
					result.Picked = std::move(material);
					result.SourcePath = absolute_path;
				}
			}
		}

		return result;
	}
}
