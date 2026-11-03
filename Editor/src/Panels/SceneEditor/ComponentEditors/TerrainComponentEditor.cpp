#include "Pch.h"
#include "ComponentEditors.h"
#include "UI/ImGuiExtensions.h"
#include "Common/TextMatch.h"
#include "UI/MaterialSlotRow.h"
#include "Panels/SceneEditor/TerrainSculpt.h"
#include "Helios/Core/AssetManager.h"
#include "Helios/Common/PathUtils.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include "Helios/Scene/Components.h"
#include "Helios/Scene/TerrainAsset.h"

namespace Helios
{
	namespace
	{
		/* 引用输入框：只读展示名字，靠拖放赋值（与模型卡同一套交互） */
		void DrawRefInputBox(const char* id, const std::string& text, float width, const char* hint)
		{
			char buffer[256] = {};
			std::strncpy(buffer, text.c_str(), sizeof(buffer) - 1);

			ImGui::SetNextItemWidth(width);
			ImGui::InputTextWithHint(id, hint, buffer, sizeof(buffer), ImGuiInputTextFlags_ReadOnly);
		}

		/* 绑资产：把数据侧的参数抄进组件，面板上的数字跟实际用的对得上。
		 * 材质只在还挂着内建默认时才跟着换，用户自己挑过的不覆盖 */
		void BindAsset(TerrainComponent& component, const std::string& absolute_path)
		{
			TerrainAsset::Data data;
			if (!TerrainAsset::Read(absolute_path, data))
				return;

			component.m_AssetPath = absolute_path;
			component.m_Extent = data.Extent;
			component.m_Resolution = data.Resolution;
			component.m_HeightScale = data.HeightScale;

			/* 没有来源路径就是内建那份（每块地形自己一份副本，比不了指针） */
			const bool use_builtin_material = component.m_MaterialSlot.pMaterial == nullptr
				|| component.m_MaterialSlot.SourceAssetPath.empty();
			if (use_builtin_material)
			{
				const SharedPtr<Material> asset_material = data.MaterialPath.empty()
					? nullptr : AssetManager::Instance().GetOrLoad<Material>(ABSOLUTE_PATH(data.MaterialPath));
				if (asset_material != nullptr)
				{
					component.m_MaterialSlot.pMaterial = asset_material;
					component.m_MaterialSlot.SourceAssetPath = ABSOLUTE_PATH(data.MaterialPath);
					component.m_MaterialSlot.IsInstance = false;
				}
			}
		}

		/* 地形资产行：拖 .terrain 进来换数据源，Clear 回到程序化生成 */
		bool DrawTerrainAssetRow(TerrainComponent& component)
		{
			bool changed = false;

			std::string asset_name = "Procedural";
			std::string asset_path;
			if (!component.m_AssetPath.empty())
			{
				asset_name = PathToUtf8(PathFromUtf8(component.m_AssetPath).filename());
				asset_path = RELATIVE_PATH(component.m_AssetPath);
			}

			const float value_width = ImGuiExt::BeginPropertyRow("Asset");
			const float frame_height = ImGui::GetFrameHeight();
			const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
			const bool has_asset = !component.m_AssetPath.empty();
			const float clear_width = has_asset
				? ImGui::CalcTextSize("Clear").x + ImGui::GetStyle().FramePadding.x * 2.0f
				: 0.0f;
			const float target_width = has_asset
				? std::max(1.0f, value_width - clear_width - gap)
				: value_width;

			DrawRefInputBox("##TerrainAssetRef", asset_name, target_width, "Drop a .terrain here");

			if (ImGui::IsItemHovered())
			{
				if (has_asset)
					ImGui::SetTooltip("%s\n\nDrop another .terrain to replace it, or clear to go back to the procedural heightmap",
						asset_path.c_str());
				else
					ImGui::SetTooltip("Heightmap comes from the component parameters.\nDrop a .terrain asset to drive it from a file");
			}

			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RESOURCE_BROWSER_ITEM"))
				{
					std::filesystem::path relative_path;
					if (payload->DataSize > 1 && TryPathFromUtf8Payload(
						payload->Data, static_cast<size_t>(payload->DataSize), relative_path)
						&& LowercaseExtension(relative_path) == ".terrain")
					{
						BindAsset(component, PathToUtf8(ResolveAssetPath(relative_path)));
						changed = true;
					}
				}
				ImGui::EndDragDropTarget();
			}

			if (has_asset)
			{
				ImGui::SameLine(0.0f, gap);
				if (ImGui::Button("Clear", ImVec2(clear_width, frame_height)))
				{
					component.m_AssetPath.clear();
					changed = true;
				}
			}

			ImGuiExt::EndPropertyRow();
			return changed;
		}

		/* 材质槽：与草地卡共用版式；清掉回资产的材质，资产也没有才回内建默认 */
		bool DrawTerrainMaterialBlock(TerrainComponent& component)
		{
			MaterialSlotOverride& slot = component.m_MaterialSlot;

			const std::string material_path = !slot.SourceAssetPath.empty()
				? slot.SourceAssetPath
				: (slot.pMaterial != nullptr ? slot.pMaterial->GetPath() : std::string());

			MaterialSlotRow::State state;
			state.Label = "Material";
			state.Path = material_path;
			state.EmptyText = "Built-in default";
			state.HoverPath = material_path.empty() ? std::string() : RELATIVE_PATH(material_path);
			state.IsInstance = slot.IsInstance;
			state.CanClear = !material_path.empty();
			state.EmptyHint = "Drop a .mtl here";
			state.ClearHint = "Clear the source and fall back to the default look";
			state.ResetHint = "Clone again from the source .mtl";

			const MaterialSlotRow::Result row = MaterialSlotRow::Draw(state);
			switch (row.Act)
			{
			case MaterialSlotRow::Action::Assign:
				slot.pMaterial = row.Picked;
				slot.SourceAssetPath = row.SourcePath;
				slot.IsInstance = false;
				return true;
			case MaterialSlotRow::Action::Clear:
			{
				/* 优先回资产自带的材质，没有再落到内建默认 */
				SharedPtr<Material> fallback = TerrainComponent::GetDefaultMaterial();
				if (!component.m_AssetPath.empty())
				{
					TerrainAsset::Data data;
					if (TerrainAsset::Read(component.m_AssetPath, data) && !data.MaterialPath.empty())
					{
						if (const auto asset_material = AssetManager::Instance().GetOrLoad<Material>(
							ABSOLUTE_PATH(data.MaterialPath)))
							fallback = asset_material;
					}
				}

				slot.pMaterial = std::move(fallback);
				slot.SourceAssetPath.clear();
				slot.IsInstance = false;
				return true;
			}
			case MaterialSlotRow::Action::Reset:
			{
				auto source = AssetManager::Instance().GetOrLoad<Material>(slot.SourceAssetPath);
				if (source == nullptr)
					return false;

				slot.pMaterial = source->Clone();
				slot.IsInstance = true;
				return true;
			}
			default:
				return false;
			}
		}

		/* 笔刷与导出：工具状态是编辑器级的（不进组件字段，也不进撤销），
		 * 真正改高度的是视口那边，这里只切模式、调参、发导出请求 */
		void DrawTerrainSculptBlock(const TerrainComponent& component)
		{
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::TextUnformatted("Sculpt");

			TerrainSculpt::Settings& settings = TerrainSculpt::GetSettings();
			const char* labels[] = { "Off", "Raise", "Lower", "Smooth" };
			for (int index = 0; index < 4; ++index)
			{
				if (index > 0)
					ImGui::SameLine();

				const bool selected = static_cast<int>(settings.Brush) == index;
				if (selected)
					ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
				if (ImGui::Button(labels[index]))
					settings.Brush = static_cast<TerrainSculpt::Mode>(index);
				if (selected)
					ImGui::PopStyleColor();
			}

			const float value_width = ImGuiExt::BeginPropertyRow("Radius");
			ImGui::SetNextItemWidth(value_width);
			ImGui::DragFloat("##TerrainSculptRadius", &settings.Radius, 0.5f, 1.0f, 200.0f, "%.1f m");
			ImGuiExt::EndPropertyRow();

			ImGui::SetNextItemWidth(ImGuiExt::BeginPropertyRow("Strength"));
			ImGui::DragFloat("##TerrainSculptStrength", &settings.Strength, 0.2f, 0.2f, 60.0f, "%.1f m/s");
			ImGuiExt::EndPropertyRow();

			if (settings.Brush != TerrainSculpt::Mode::None)
			{
				ImGui::TextDisabled("Drag in the viewport to paint the selected terrain");
			}

			ImGui::Spacing();
			const bool procedural = component.m_AssetPath.empty();
			if (ImGui::Button("Export as .terrain"))
				TerrainSculpt::RequestExport();
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Write <project>/Terrains/<entity>.terrain and bind it to this terrain");

			ImGui::SameLine();
			ImGui::TextDisabled(procedural ? "Procedural heights" : "Already bound to an asset");
		}

		bool DrawTerrainBlock(void* raw)
		{
			auto& component = *static_cast<TerrainComponent*>(raw);

			bool changed = DrawTerrainAssetRow(component);
			changed |= DrawTerrainMaterialBlock(component);
			DrawTerrainSculptBlock(component);
			return changed;
		}
	}

	void RegisterTerrainComponentEditor()
	{
		ComponentRegistry::Instance().SetCustomDraw(typeid(TerrainComponent), &DrawTerrainBlock);
	}
}
