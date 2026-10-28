#include "Pch.h"
#include "ComponentEditors.h"
#include "Common/TextMatch.h"
#include "Helios/Core/AssetManager.h"
#include "ImGui/EditorTheme.h"
#include "ImGui/ImGuiExtensions.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include "Helios/Scene/Components.h"
#include "Helios/Scene/SceneCommon.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>

namespace Helios
{
	namespace
	{
		/* ==================== 材质槽区块 ====================
		 * 每槽一行（槽名 | 绑定的材质文件 + 拖拽挂载 / 清除 / 重置）= Model 卡里的"材质引用"。
		 * 材质详情（Shader 和参数）在 SceneHierarchy 的"材质卡"里（通道不能嵌套、只能同级平铺）。 */

		/* 资源引用输入框：外观是标准输入框、只读展示资源名（文本可选中复制），
		 * 拖放目标与 tooltip 由调用方挂在控件上；空文本时显示 hint（弱化色）。 */
		void DrawRefInputBox(const char* id, const std::string& text, float width, const char* hint)
		{
			char buffer[256] = {};
			std::strncpy(buffer, text.c_str(), sizeof(buffer) - 1);

			ImGui::SetNextItemWidth(width);
			ImGui::InputTextWithHint(id, hint, buffer, sizeof(buffer), ImGuiInputTextFlags_ReadOnly);
		}

		/* 槽行：槽名 | 有效材质（.mtl，只读展示 + 拖拽挂载）+ 清除 / 重置。
		 * 有效材质 = 实体绑定 ?: 模型清单默认 ?: 内置白模；都没有时留空、hint 提示可拖入。 */
		bool DrawMaterialSlotRow(ModelComponent& component, const Model& model, int slot_index)
		{
			const MaterialSlot* slot = model.GetSlotByIndex(slot_index);
			if (slot == nullptr)
				return false;

			bool changed = false;

			const auto iter = component.m_SlotOverrides.find(slot->Name);
			const bool has_override = (iter != component.m_SlotOverrides.end() && iter->second.pMaterial != nullptr);
			const bool is_instance = has_override && iter->second.IsInstance;

			/* 有效材质来自哪份 .mtl（覆盖优先；模型清单默认次之） */
			std::string material_path;
			if (has_override)
			{
				material_path = !iter->second.SourceAssetPath.empty()
					? iter->second.SourceAssetPath
					: iter->second.pMaterial->GetPath();
			}
			else
			{
				material_path = slot->SourcePath;
			}

			const std::string display = material_path.empty()
				? std::string()
				: PathToUtf8(PathFromUtf8(material_path).filename());

			const float gap = ImGui::GetStyle().ItemInnerSpacing.x;

			const float value_width = ImGuiExt::BeginPropertyRow(slot->Name.c_str());

			/* 行内自算尺寸（X / Reset 按钮高）必须在 Begin 之后取：行高由
			 * BeginPropertyRow 统一压成"字体高"，这里要跟行内实际控件一致 */
			const float frame_height = ImGui::GetFrameHeight();

			/* 右端按钮区：清除（X）+ 有来源时的重置 */
			const bool can_reset = is_instance && !iter->second.SourceAssetPath.empty();
			const float reset_width = can_reset
				? ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2.0f
				: 0.0f;
			float button_zone = has_override ? (frame_height + gap) : 0.0f;
			if (can_reset)
				button_zone += reset_width + gap;

			/* 实例标记放值列开头：多槽时一眼分出哪几行是改出来的副本，占的宽度从文件名里扣 */
			float chip_width = 0.0f;
			if (is_instance)
			{
				chip_width = ImGuiExt::DrawTagChip("Instance", frame_height) + gap;
				ImGui::SameLine(0.0f, gap);
			}

			const float target_width = std::max(1.0f, value_width - button_zone - chip_width);

			DrawRefInputBox("##MaterialRef", display, target_width, "Drop .mtl here");

			if (ImGui::IsItemHovered())
			{
				/* 槽位上挂的是改过的副本：本行只显示来源 .mtl，改的值不在那份文件里 */
				std::string tooltip = material_path.empty()
					? std::string("Drag a .mtl material asset here")
					: RELATIVE_PATH(material_path) + "\n\nDrag a .mtl material asset here";
				if (is_instance)
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
						const std::string extension = LowercaseExtension(relative_path);

						if (extension == ".mtl")
						{
							const std::string absolute_path = PathToUtf8(ResolveAssetPath(relative_path));
							auto material = AssetManager::Instance().GetOrLoad<Material>(absolute_path);
							if (material != nullptr)
							{
								MaterialSlotOverride slot_override;
								slot_override.pMaterial = std::move(material);
								slot_override.SourceAssetPath = absolute_path;
								component.m_SlotOverrides[slot->Name] = std::move(slot_override);
								changed = true;
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
				{
					/* 来源是模型默认（含清单里的参数覆盖）就克隆模型默认；否则回来源资产 */
					const bool from_model_default = iter->second.SourceAssetPath == slot->SourcePath;
					SharedPtr<Material> source = (from_model_default && slot->pDefault != nullptr)
						? slot->pDefault
						: AssetManager::Instance().GetOrLoad<Material>(iter->second.SourceAssetPath);
					if (source != nullptr)
					{
						auto& slot_override = component.m_SlotOverrides[slot->Name];
						slot_override.pMaterial = source->Clone();
						slot_override.IsInstance = true;
						changed = true;
					}
				}

				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Re-clone from the model default or source asset");
			}

			if (has_override)
			{
				ImGui::SameLine(0.0f, gap);
				if (ImGui::Button("X", ImVec2(frame_height, frame_height)))
				{
					component.m_SlotOverrides.erase(slot->Name);
					changed = true;
				}

				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Clear the binding, fall back to the model default");
			}

			ImGuiExt::EndPropertyRow();
			return changed;
		}

		/* 材质槽区块入口：标题 + 统计 + 每槽一行引用（详情在 SceneHierarchy 的材质卡里） */
		bool DrawMaterialSlots(ModelComponent& component, const Model& model)
		{
			const auto& slots = model.GetMaterialSlots();
			if (slots.empty())
				return false;

			bool changed = false;

			/* 统计口径按"实例"说：改过的槽位是就地克隆的副本，不是资产本身 */
			int instance_count = 0;
			for (const auto& slot : slots)
			{
				const auto iter = component.m_SlotOverrides.find(slot.Name);
				if (iter != component.m_SlotOverrides.end() && iter->second.IsInstance
					&& iter->second.pMaterial != nullptr)
					++instance_count;
			}

			ImGui::Spacing();
			ImGui::TextColored(EditorTheme::Token::TextLabel, "Materials");
			ImGui::SameLine();
			/* 统计文案窄窗里必须能折行：裸文本的布局宽度不受裁剪影响，会撑出横向滚动 */
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
			ImGui::TextDisabled("(%d slot%s, %d instance%s)", static_cast<int>(slots.size()),
				slots.size() == 1 ? "" : "s", instance_count, instance_count == 1 ? "" : "s");
			ImGui::PopTextWrapPos();

			for (int i = 0; i < static_cast<int>(slots.size()); ++i)
			{
				ImGui::PushID(i);
				changed |= DrawMaterialSlotRow(component, model, i);
				ImGui::PopID();
			}

			return changed;
		}

		bool DrawModelBlock(void* raw)
		{
			auto& component = *static_cast<ModelComponent*>(raw);
			auto& model = component.m_Model;
			bool changed = false;

			/* 名称与路径：内建模型显示身份名（BuiltinSphere 等）——它不是磁盘上的
			 * 文件，拿去做 RELATIVE_PATH 只会得到一串上跳相对路径。 */
			std::string model_name;
			std::string model_path;
			if (model != nullptr)
			{
				BuiltinModelType builtin_type{};
				if (model->TryGetBuiltinType(builtin_type))
				{
					model_name = model->GetPath();
					model_path = model_name;
				}
				else
				{
					model_name = PathToUtf8(PathFromUtf8(model->GetPath()).filename());
					model_path = RELATIVE_PATH(model->GetPath());
				}
			}

			/* 模型引用输入框：只读展示模型名，拖放 .mesh / .model 直接落在输入框上；
			 * 完整路径在 tooltip 里给出（输入框只放名字，长路径不进布局） */
			const float value_width = ImGuiExt::BeginPropertyRow("Model");
			const float frame_height = ImGui::GetFrameHeight();
			const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
			const float clear_width = model != nullptr
				? ImGui::CalcTextSize("Clear").x + ImGui::GetStyle().FramePadding.x * 2.0f
				: 0.0f;
			const float target_width = model != nullptr
				? std::max(1.0f, value_width - clear_width - gap)
				: value_width;

			DrawRefInputBox("##ModelRef", model_name, target_width, "Drop .mesh / .model here");

			if (ImGui::IsItemHovered())
			{
				if (model != nullptr)
					ImGui::SetTooltip("%s\n\nDrag a .mesh or .model asset from the Resource Browser to assign or replace the model",
						model_path.c_str());
				else
					ImGui::SetTooltip("Drag a .mesh or .model asset from the Resource Browser to assign the model");
			}

			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RESOURCE_BROWSER_ITEM"))
				{
					std::filesystem::path relative_path;
					if (payload->DataSize > 1 && TryPathFromUtf8Payload(
						payload->Data, static_cast<size_t>(payload->DataSize), relative_path))
					{
						const std::string extension = LowercaseExtension(relative_path);

						/* .mesh 是纯几何，.model 是带材质槽的清单，两种都能直接挂 */
						if (extension == ".mesh" || extension == ".model")
						{
							const std::string absolute_path = PathToUtf8(ResolveAssetPath(relative_path));
							if (model == nullptr || model->GetPath() != absolute_path)
							{
								SharedPtr<Model> loaded_model = Model::Create(absolute_path);
								if (loaded_model != nullptr)
								{
									model = std::move(loaded_model);
									changed = true;
								}
							}
						}
					}
				}
				ImGui::EndDragDropTarget();
			}

			if (model != nullptr)
			{
				ImGui::SameLine(0.0f, gap);
				if (ImGui::Button("Clear", ImVec2(clear_width, frame_height)))
				{
					model.reset();
					changed = true;
				}
			}

			ImGuiExt::EndPropertyRow();

			/* 投影 / 接受阴影：渲染端的逐对象开关（见 VisibleMeshObject 与阴影 Pass / 光照采样） */
			if (ImGuiExt::DrawCheckboxUI("CastShadow", component.m_CastShadow))
				changed = true;
			if (ImGuiExt::DrawCheckboxUI("ReceiveShadow", component.m_ReceiveShadow))
				changed = true;

			/* 带骨骼的模型才给这个开关：关掉按绑定姿态画（排查 / 省性能） */
			if (model != nullptr && model->HasSkin())
			{
				if (ImGuiExt::DrawCheckboxUI("Skinning", component.m_EnableSkinning))
					changed = true;
			}

			/* 材质槽区块：每槽一行 + 当前材质属性（可就地编辑） */
			if (model != nullptr)
				changed |= DrawMaterialSlots(component, *model);

			return changed;
		}
	}

	void RegisterModelComponentEditor()
	{
		ComponentRegistry::Instance().SetCustomDraw(typeid(ModelComponent), &DrawModelBlock);
	}
}
