#include "Pch.h"
#include "ComponentEditors.h"
#include "UI/ImGuiExtensions.h"
#include "UI/EditorIcons.h"
#include "UI/EditorTheme.h"
#include "Common/TextMatch.h"
#include "UI/MaterialSlotRow.h"
#include "Panels/SceneEditor/TerrainSculpt.h"
#include "Panels/SceneEditor/SceneInspector.h"
#include "UI/PanelChrome.h"
#include "Material/MaterialParamUI.h"
#include "Command/ComponentFieldCommand.h"
#include <imgui_internal.h>
#include "Helios/Core/AssetManager.h"
#include "Helios/Common/PathUtils.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include "Helios/Scene/Components.h"
#include "Helios/Scene/TerrainField.h"
#include "Helios/Scene/TerrainAsset.h"

namespace Helios
{
	namespace
	{
		class TerrainMaterialCommand final : public ICommand
		{
		public:
			TerrainMaterialCommand(Entity entity, const InstanceMaterialSlot& before, const InstanceMaterialSlot& after)
				: m_Entity(entity), m_Before(before), m_After(after) {}
			void Do() override { Apply(m_After); }
			void Undo() override { Apply(m_Before); }
			const char* GetLabel() const override { return "Edit Terrain Material"; }
			bool TryMerge(const ICommand& next) override
			{
				const auto* command = dynamic_cast<const TerrainMaterialCommand*>(&next);
				if (command == nullptr || command->m_Entity != m_Entity)
					return false;
				m_After = command->m_After;
				return true;
			}
		private:
			void Apply(const InstanceMaterialSlot& slot)
			{
				if (m_Entity.HasComponent<TerrainComponent>())
					m_Entity.GetComponent<TerrainComponent>().m_MaterialSlot = slot;
			}
			Entity m_Entity;
			InstanceMaterialSlot m_Before, m_After;
		};

		class TerrainExpandCommand final : public ICommand
		{
		public:
			TerrainExpandCommand(Entity entity, SharedPtr<TerrainField> field,
				TerrainField::HeightState before, TerrainField::HeightState after)
				: m_Entity(entity), m_Field(std::move(field)), m_Before(std::move(before)), m_After(std::move(after)) {}
			void Do() override
			{
				if (m_FirstApply)
					m_FirstApply = false;
				else
					Apply(m_After);
			}
			void Undo() override { Apply(m_Before); }
			const char* GetLabel() const override { return "Expand Terrain"; }
		private:
			void Apply(const TerrainField::HeightState& state)
			{
				if (!m_Entity.HasComponent<TerrainComponent>())
					return;
				auto& component = m_Entity.GetComponent<TerrainComponent>();
				if (component.m_Field == m_Field)
					m_Field->RestoreHeightState(component, state);
			}
			Entity m_Entity;
			SharedPtr<TerrainField> m_Field;
			TerrainField::HeightState m_Before, m_After;
			bool m_FirstApply{ true };
		};

		bool IsGenerationField(const FieldDesc& field)
		{
			return std::strcmp(field.Name, "Extent") == 0 || std::strcmp(field.Name, "Resolution") == 0
				|| std::strcmp(field.Name, "HeightScale") == 0 || std::strcmp(field.Name, "Seed") == 0;
		}

		/* 绑资产：把数据侧的参数抄进组件，面板上的数字跟实际用的对得上。
		 * 材质只在还挂着内建默认时才跟着换，用户自己挑过的不覆盖 */
		bool BindAsset(TerrainComponent& component, const std::string& absolute_path)
		{
			TerrainAsset::Data data;
			if (!TerrainAsset::Read(absolute_path, data))
				return false;

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
			return true;
		}

		/* 地形资产行：拖 .terrain 进来换数据源，清除回到程序化生成 */
		bool DrawTerrainAssetRow(TerrainComponent& component, bool editable = true)
		{
			bool changed = false;

			std::string asset_name = "Procedural";
			std::string asset_path;
			if (!component.m_AssetPath.empty())
			{
				asset_name = PathToUtf8(PathFromUtf8(component.m_AssetPath).filename());
				asset_path = RELATIVE_PATH(component.m_AssetPath);
			}

			const bool has_asset = !component.m_AssetPath.empty();

			ImGui::BeginDisabled(!editable);
			ImGuiExt::AssetRefRowOptions options;
			options.Label = "Asset";
			options.Text = asset_name;
			options.Hint = "Drop a .terrain here";
			options.ShowClear = has_asset;
			options.ClearHint = "Clear the terrain asset and use procedural heights";
			options.Tooltip = has_asset
				? asset_path + "\n\nDrop another .terrain to replace it, or clear to go back to the procedural heightmap"
				: std::string("Heightmap comes from the component parameters.\nDrop a .terrain asset to drive it from a file");

			const ImGuiExt::AssetRefRowResult row = ImGuiExt::DrawAssetRefRow(options);
			ImGui::EndDisabled();

			if (row.Clear)
			{
				component.m_AssetPath.clear();
				changed = true;
			}

			if (!row.DroppedPath.empty())
			{
				const std::filesystem::path relative_path = PathFromUtf8(row.DroppedPath);
				if (LowercaseExtension(relative_path) == ".terrain")
					changed = BindAsset(component, PathToUtf8(ResolveAssetPath(relative_path)));
			}

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
				/* 优先回资产自带的材质（按引用挂，地形数据走逐绘制覆盖）；
				 * 没有再落到内建默认的副本 —— 高度图这些参数按地形写，共用会互相覆盖 */
				slot.IsInstance = false;
				if (!component.m_AssetPath.empty())
				{
					TerrainAsset::Data data;
					if (TerrainAsset::Read(component.m_AssetPath, data) && !data.MaterialPath.empty())
					{
						const std::string material_path = ABSOLUTE_PATH(data.MaterialPath);
						if (auto asset_material = AssetManager::Instance().GetOrLoad<Material>(material_path))
						{
							slot.pMaterial = std::move(asset_material);
							slot.SourceAssetPath = material_path;
							return true;
						}
					}
				}

				const SharedPtr<Material> fallback = TerrainComponent::GetDefaultMaterial();
				slot.pMaterial = fallback != nullptr ? fallback->Clone() : nullptr;
				slot.SourceAssetPath.clear();
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

		void DrawTerrainSculptBlock(const TerrainComponent& component, Entity entity = {})
		{
			TerrainSculpt::Settings& settings = TerrainSculpt::GetSettings();
			constexpr Icons::Id icons[] = {
				Icons::Id::TerrainNoop, Icons::Id::TerrainRaise, Icons::Id::TerrainLower, Icons::Id::TerrainSmooth,
			};
			constexpr const char* labels[] = { "Off", "Raise", "Lower", "Smooth" };
			constexpr const char* tips[] = { "Stop sculpting", "Raise terrain", "Lower terrain", "Smooth nearby heights" };
			const float gap = EditorTheme::Scaled(4.0f);
			const float height = ImGui::GetFrameHeight();
			const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x - height - gap);
			const int brush = Icons::IconToggleGroup("##TerrainSculptMode", icons, tips, IM_ARRAYSIZE(icons),
				static_cast<int>(settings.Brush), ImVec2(width, height), labels);
			if (brush >= 0)
				settings.Brush = static_cast<TerrainSculpt::Mode>(brush);
			ImGui::SameLine(0.0f, gap);
			const bool ready = component.m_Field != nullptr && !component.m_Field->IsEmpty();
			ImGui::BeginDisabled(!entity || !ready || TerrainSculpt::GetExportRequest().Status == TerrainSculpt::ExportStatus::Pending);
			if (Icons::IconButton(Icons::Id::Export, ImVec2(height, height), false,
				"Export heights to the bound asset or create a new .terrain"))
				TerrainSculpt::RequestExport(entity);
			ImGui::EndDisabled();
			ImGui::BeginDisabled(settings.Brush == TerrainSculpt::Mode::None);
			ImGuiExt::DrawSliderFloatUI("Radius", settings.Radius, 1.0f, 200.0f,
				EditorTheme::Token::PropertyLabelWidth, "%.1f m");
			const bool smooth = settings.Brush == TerrainSculpt::Mode::Smooth;
			ImGuiExt::DrawSliderFloatUI(smooth ? "Smooth rate" : "Strength", settings.Strength, 0.2f, 60.0f,
				EditorTheme::Token::PropertyLabelWidth, smooth ? "%.1f /s" : "%.1f m/s");
			ImGui::EndDisabled();
			ImGui::PushTextWrapPos(0.0f);
			if (component.m_Field == nullptr || component.m_Field->IsEmpty())
				ImGui::TextDisabled("Height data is not ready for sculpting.");
			ImGui::PopTextWrapPos();
		}

		bool DrawTerrainBlock(void* raw)
		{
			DrawTerrainSculptBlock(*static_cast<TerrainComponent*>(raw));
			return false;
		}
	}

	void SceneInspector::DrawTerrainField(const ComponentDesc& desc, Entity& entity, TerrainComponent& component,
		const FieldDesc& field)
	{
		const bool generation = IsGenerationField(field);
		const bool rebuild = generation || std::strcmp(field.Name, "LodRatio") == 0 || std::strcmp(field.Name, "MinNodeSize") == 0;
		const bool dirty = component.m_Field != nullptr && component.m_Field->HasUnsavedEdits();
		const bool disabled = (generation && !component.m_AssetPath.empty()) || m_TerrainPanel.PendingField != nullptr;
		ImGui::PushID(field.Name);
		ImGui::BeginDisabled(disabled);
		DrawEditableField(desc, entity, &component, field, [&](const FieldDesc& metadata, void* storage)
		{
			alignas(16) uint8_t value[kFieldValueCapacity]{};
			std::memcpy(value, storage, metadata.ValueSize);
			const char* label = metadata.Name;
			if (std::strcmp(label, "Extent") == 0)
			{
				glm::vec2 size = *reinterpret_cast<glm::vec2*>(value) * 2.0f;
				ImGui::SetNextItemWidth(ImGuiExt::BeginPropertyRow("Size"));
				ImGui::DragFloat2("##Size", &size.x, metadata.DragSpeed, 1.0f, 0.0f, "%.1f m");
				ImGuiExt::EndPropertyRow();
				*reinterpret_cast<glm::vec2*>(value) = size * 0.5f;
			}
			else
			{
				FieldDesc display = metadata;
				if (std::strcmp(label, "HeightScale") == 0) display.Name = "Height scale";
				else if (std::strcmp(label, "Seed") == 0) display.Name = "Random seed";
				else if (std::strcmp(label, "LodRatio") == 0) display.Name = "LOD ratio";
				else if (std::strcmp(label, "MinNodeSize") == 0) display.Name = "Min patch size";
				else if (std::strcmp(label, "ReceiveShadow") == 0) display.Name = "Receive shadows";
				else if (std::strcmp(label, "CastShadow") == 0) display.Name = "Cast shadows";
				DrawFieldControl(display, value);
			}

			if (std::memcmp(value, storage, metadata.ValueSize) == 0 || disabled)
				return;
			if (dirty && rebuild)
			{
				m_TerrainPanel.PendingField = &field;
				std::memcpy(m_TerrainPanel.Before, storage, metadata.ValueSize);
				std::memcpy(m_TerrainPanel.After, value, metadata.ValueSize);
			}
			else
				std::memcpy(storage, value, metadata.ValueSize);
		});
		ImGui::EndDisabled();
		ImGui::PopID();
	}

	void SceneInspector::DrawTerrainRebuildPrompt(const ComponentDesc& desc, Entity& entity, TerrainComponent& component)
	{
		if (m_TerrainPanel.PendingField == nullptr)
			return;
		const auto& request = TerrainSculpt::GetExportRequest();
		const bool waiting = m_TerrainPanel.ExportSerial != 0 && request.Serial == m_TerrainPanel.ExportSerial
			&& request.Status == TerrainSculpt::ExportStatus::Pending;
		if (m_TerrainPanel.ExportSerial != 0 && request.Serial == m_TerrainPanel.ExportSerial
			&& request.Target == entity && request.Status == TerrainSculpt::ExportStatus::Succeeded)
		{
			const FieldDesc& field = *m_TerrainPanel.PendingField;
			const void* live = reinterpret_cast<const uint8_t*>(&component) + field.Offset;
			if (std::memcmp(live, m_TerrainPanel.Before, field.ValueSize) == 0
				&& component.m_Field != nullptr && !component.m_Field->HasUnsavedEdits())
			{
				if (m_pCommandStack != nullptr)
					m_pCommandStack->Execute(CreateUniquePtr<ComponentFieldCommand>(entity, desc, field,
						m_TerrainPanel.Before, m_TerrainPanel.After));
				else
					std::memcpy(reinterpret_cast<uint8_t*>(&component) + field.Offset, m_TerrainPanel.After, field.ValueSize);
			}
			m_TerrainPanel.PendingField = nullptr;
			m_TerrainPanel.ExportSerial = 0;
			if (ImGui::BeginPopupModal("##TerrainRebuild", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
			{
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
			}
			return;
		}

		ImGui::OpenPopup("##TerrainRebuild");
		ImGui::SetNextWindowSizeConstraints(ImVec2(EditorTheme::Scaled(280.0f), 0.0f), ImVec2(EditorTheme::Scaled(400.0f), FLT_MAX));
		if (ImGui::BeginPopupModal("##TerrainRebuild", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
		{
			ImGui::TextUnformatted("Save heights before rebuilding");
			ImGui::PushTextWrapPos(EditorTheme::Scaled(300.0f));
			ImGui::TextDisabled("Changing %s reloads height data. Export the current sculpt first. Generator changes create a new procedural terrain; the exported sculpt remains in Terrains.",
				m_TerrainPanel.PendingField->Name);
			if (m_TerrainPanel.ExportSerial != 0 && request.Status == TerrainSculpt::ExportStatus::Failed)
				ImGui::TextColored(EditorTheme::Token::Danger, "Export failed. The parameter was not changed.");
			ImGui::PopTextWrapPos();
			ImGui::BeginDisabled(waiting);
			if (ImGui::Button("Cancel"))
			{
				m_TerrainPanel.PendingField = nullptr;
				m_TerrainPanel.ExportSerial = 0;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Export and apply"))
			{
				const char* name = m_TerrainPanel.PendingField->Name;
				const bool generation = std::strcmp(name, "Extent") == 0 || std::strcmp(name, "Resolution") == 0
					|| std::strcmp(name, "HeightScale") == 0 || std::strcmp(name, "Seed") == 0;
				m_TerrainPanel.ExportSerial = TerrainSculpt::RequestExport(entity, !generation);
			}
			ImGui::EndDisabled();
			if (waiting)
				ImGui::TextDisabled("Exporting heights...");
			ImGui::EndPopup();
		}
	}

	bool SceneInspector::DrawTerrainExpandBlock(Entity entity, TerrainComponent& component)
	{
		const SharedPtr<TerrainField>& field = component.m_Field;
		const bool ready = field != nullptr && !field->IsEmpty();
		if (!ready)
		{
			ImGui::TextDisabled("Height data is not ready.");
			return false;
		}
		const glm::vec2 size = field->GetExtent() * 2.0f;
		if (size != m_TerrainPanel.ExpandSourceSize)
		{
			m_TerrainPanel.ExpandSourceSize = size;
			m_TerrainPanel.ExpandTargetSize = size * 1.25f;
			m_TerrainPanel.ExpandFailed = false;
		}
		bool expanded = false;
		const bool pending = m_TerrainPanel.PendingField != nullptr;
		ImGui::BeginDisabled(pending);
		ImGui::SetNextItemWidth(ImGuiExt::BeginPropertyRow("New size"));
		if (ImGui::InputFloat2("##ExpandSize", &m_TerrainPanel.ExpandTargetSize.x, "%.1f"))
			m_TerrainPanel.ExpandFailed = false;
		ImGuiExt::EndPropertyRow();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Full local width X and length Z in meters. Existing heights are resampled in place.");
		const uint32_t resolution = field->GetExpansionResolution(m_TerrainPanel.ExpandTargetSize * 0.5f);
		if (resolution != 0)
		{
			ImGuiExt::BeginPropertyRow("Samples");
			ImGui::Text("%u x %u", resolution, resolution);
			ImGuiExt::EndPropertyRow();
		}
		else
		{
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextDisabled("Increase at least one side without reducing the other. Sampling is limited to 4096 per axis.");
			ImGui::PopTextWrapPos();
		}
		ImGui::BeginDisabled(resolution == 0);
		if (ImGui::Button("Expand", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)))
		{
			ServiceComponentEditTransaction();
			auto before = field->CaptureHeightState();
			expanded = field->Expand(component, m_TerrainPanel.ExpandTargetSize * 0.5f);
			m_TerrainPanel.ExpandFailed = !expanded;
			if (expanded && m_pCommandStack != nullptr)
				m_pCommandStack->Execute(CreateUniquePtr<TerrainExpandCommand>(entity, field,
					std::move(before), field->CaptureHeightState()));
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Extend both sides around the current center. New areas use edge heights. Export or save the scene to write the asset.");
		if (m_TerrainPanel.ExpandFailed)
			ImGui::TextColored(EditorTheme::Token::Danger, "Expansion failed. The terrain was not changed.");
		ImGui::EndDisabled();
		return expanded && m_pCommandStack == nullptr;
	}

	bool SceneInspector::DrawTerrainPanel(const ComponentDesc& desc, Entity& entity, TerrainComponent& component)
	{
		if (m_TerrainPanel.Target != entity)
		{
			m_TerrainPanel = {};
			m_TerrainPanel.Target = entity;
		}
		const bool ready = component.m_Field != nullptr && !component.m_Field->IsEmpty();
		const bool dirty = ready && component.m_Field->HasUnsavedEdits();
		const auto& request = TerrainSculpt::GetExportRequest();
		const bool exporting = request.Status == TerrainSculpt::ExportStatus::Pending;
		const bool failed = request.Target == entity && request.Status == TerrainSculpt::ExportStatus::Failed;
		bool changed = false;

		ImGui::Spacing();
		changed |= DrawTerrainAssetRow(component, !dirty && !exporting);
		if (dirty)
		{
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextDisabled("Export sculpt edits before replacing the asset.");
			ImGui::PopTextWrapPos();
		}
		ImGui::Spacing();

		ImGui::BeginDisabled(exporting);
		if (MaterialParamUI::BeginPropertyGroup("##Sculpt", "Sculpt"))
			DrawTerrainSculptBlock(component, entity);
		if (MaterialParamUI::BeginPropertyGroup("##Height", "Height data", false))
		{
			for (const FieldDesc& field : desc.Fields)
				if (IsGenerationField(field) && EvaluateCondition(field.Condition, &component))
					DrawTerrainField(desc, entity, component, field);
		}
		if (MaterialParamUI::BeginPropertyGroup("##Detail", "LOD and shadows", false))
		{
			for (const FieldDesc& field : desc.Fields)
				if (!IsGenerationField(field) && EvaluateCondition(field.Condition, &component))
					DrawTerrainField(desc, entity, component, field);
		}
		if (MaterialParamUI::BeginPropertyGroup("##Expand", "Expand bounds", false))
			changed |= DrawTerrainExpandBlock(entity, component);
		ImGui::EndDisabled();

		if (exporting || failed || dirty)
		{
			ImGui::Separator();
			ImGui::PushStyleColor(ImGuiCol_Text, failed ? EditorTheme::Token::Danger : dirty ? EditorTheme::Token::Warning : EditorTheme::Token::TextLabel);
			PanelChrome::DrawClippedTextLine(exporting ? "Exporting heights..." : failed ? "Height export failed"
				: "Unsaved sculpt edits", ImGui::GetContentRegionAvail().x);
			ImGui::PopStyleColor();
		}
		DrawTerrainRebuildPrompt(desc, entity, component);
		return changed;
	}

	void SceneInspector::DrawTerrainMaterialCard(Entity entity, TerrainComponent& component)
	{
		ImGui::PushID("##TerrainMaterial");
		const auto card = PanelChrome::BeginCard("Material · Terrain", Icons::Id::FileMaterial);
		if (component.m_MaterialSlot.IsInstance)
			PanelChrome::DrawCardBadge(card, "Instance", "Edits affect only this terrain and are saved with the scene");
		if (card.Open)
		{
			ServiceComponentEditTransaction();
			const InstanceMaterialSlot before = component.m_MaterialSlot;
			bool changed = DrawTerrainMaterialBlock(component);
			changed |= DrawTerrainMaterialBody(component);
			if (changed && m_pCommandStack != nullptr)
			{
				if (ImGui::IsAnyItemActive() && !m_ComponentEditTransactionOpen)
				{
					m_ComponentEditTransactionOpen = true;
					m_pCommandStack->BeginTransaction();
				}
				m_pCommandStack->Execute(CreateUniquePtr<TerrainMaterialCommand>(entity, before, component.m_MaterialSlot));
			}
		}
		PanelChrome::EndCard(card);
		ImGui::PopID();
	}

	void RegisterTerrainComponentEditor()
	{
		ComponentRegistry::Instance().SetCustomDraw(typeid(TerrainComponent), &DrawTerrainBlock);
	}
}
