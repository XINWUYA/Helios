#include "Pch.h"
#include "Helios/Reflection/ComponentRegistration.h"
#include "Helios/Core/AssetManager.h"
#include "Helios/ImGui/EditorTheme.h"
#include "Helios/ImGui/ImGuiExtensions.h"
#include "Helios/Scene/Components.h"
#include "Helios/Scene/SceneCommon.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <imgui.h>
#include <tinyxml2.h>

namespace Helios
{
	namespace
	{
		/* 模型是资源引用：字段表只认 POD 与字符串，路径读写单独处理；
		 * 对象为空时不写属性，读的时候也接受属性缺失。 */
		void SaveModelPath(tinyxml2::XMLElement* element, const void* component)
		{
			const auto& model_component = *static_cast<const ModelComponent*>(component);
			const auto& model = model_component.m_Model;
			if (model != nullptr)
			{
				/* 内建模型（BuiltinCube 等）没有资产文件：存身份名，加载经
				 * Model::Create 反查回 BuiltinModelType；文件模型存相对路径。 */
				BuiltinModelType builtin_type{};
				const bool is_builtin = model->TryGetBuiltinType(builtin_type);
				element->SetAttribute("ModelPath", is_builtin
					? model->GetPath().c_str()
					: RELATIVE_PATH(model->GetPath()).c_str());
			}

			/* 投影 / 接受阴影：与光源 IsCastShadow 同一口径，默认值也写出 */
			element->SetAttribute("CastShadow", model_component.m_CastShadow);
			element->SetAttribute("ReceiveShadow", model_component.m_ReceiveShadow);

			/* 槽的实体级绑定：引用形态只写路径；实例形态全量内嵌（不依赖来源资产存活） */
			for (const auto& entry : model_component.m_SlotOverrides)
			{
				const auto& slot_override = entry.second;
				if (slot_override.pMaterial == nullptr)
					continue;

				auto* override_doc = element->InsertNewChildElement("MaterialOverride");
				override_doc->SetAttribute("Slot", entry.first.c_str());

				if (slot_override.IsInstance)
				{
					override_doc->SetAttribute("Instance", true);
					if (!slot_override.SourceAssetPath.empty())
						override_doc->SetAttribute("Source", RELATIVE_PATH(slot_override.SourceAssetPath).c_str());
					MaterialIO::WriteBody(override_doc, *slot_override.pMaterial);
				}
				else
				{
					/* "覆盖"约定挂的是材质资产；运行时构造的匿名材质没有路径可存，跳过 */
					const auto& material_path = slot_override.pMaterial->GetPath();
					if (material_path.empty())
						continue;
					override_doc->SetAttribute("Asset", RELATIVE_PATH(material_path).c_str());
				}
			}
		}

		void LoadModelPath(const tinyxml2::XMLElement* element, void* component)
		{
			auto& model_component = *static_cast<ModelComponent*>(component);

			const char* path = element->Attribute("ModelPath");
			if (path != nullptr && *path != '\0')
				model_component.m_Model = Model::Create(path);

			/* 旧场景没有这两个属性：缺省即可见设定（投影 / 接受阴影都开） */
			model_component.m_CastShadow = element->BoolAttribute("CastShadow", true);
			model_component.m_ReceiveShadow = element->BoolAttribute("ReceiveShadow", true);

			/* 槽绑定：引用形态走 AssetManager；实例形态读内嵌定义 */
			model_component.m_SlotOverrides.clear();
			for (const tinyxml2::XMLElement* override_doc = element->FirstChildElement("MaterialOverride"); override_doc; override_doc = override_doc->NextSiblingElement("MaterialOverride"))
			{
				const char* slot_name = override_doc->Attribute("Slot");
				if (slot_name == nullptr || *slot_name == '\0')
					continue;

				MaterialSlotOverride slot_override;
				if (override_doc->BoolAttribute("Instance"))
				{
					auto instance = CreateSharedPtr<Material>();
					if (!MaterialIO::ReadBody(override_doc, *instance))
						continue;
					slot_override.pMaterial = std::move(instance);
					slot_override.IsInstance = true;
					if (const char* source = override_doc->Attribute("Source"))
						slot_override.SourceAssetPath = ABSOLUTE_PATH(source);
				}
				else
				{
					const char* asset = override_doc->Attribute("Asset");
					if (asset == nullptr || *asset == '\0')
						continue;
					auto material = AssetManager::Instance().GetOrLoad<Material>(ABSOLUTE_PATH(asset));
					if (material == nullptr)
					{
						CORE_LOG_WARN("Failed to load override material '{}' for slot '{}'.", asset, slot_name);
						continue;
					}
					slot_override.pMaterial = std::move(material);
				}

				model_component.m_SlotOverrides[slot_name] = std::move(slot_override);
			}
		}

		/* ==================== 材质槽区块 ====================
		 * 每槽一行（槽名 | 材质名输入框 + 挂载 / 清除 / 重置）= Model 卡里保留的"材质引用"。
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

		/* 槽行：槽名 | 材质名输入框（只读展示 + 拖拽挂载 .mtl）+ 清除 / 重置。
		 * 输入框显示材质名，实例额外带 [实例] 标记（参数独立于来源资产，要分得开）。 */
		bool DrawMaterialSlotRow(ModelComponent& component, const Model& model, int slot_index)
		{
			const MaterialSlot* slot = model.GetSlotByIndex(slot_index);
			if (slot == nullptr)
				return false;

			bool changed = false;

			const auto iter = component.m_SlotOverrides.find(slot->Name);
			const bool has_override = (iter != component.m_SlotOverrides.end() && iter->second.pMaterial != nullptr);
			const bool is_instance = has_override && iter->second.IsInstance;

			/* 值显示：材质名 + 实例标记；完整路径进 tooltip */
			std::string display;
			std::string tooltip_path;
			if (has_override)
			{
				if (is_instance)
				{
					const bool has_source = !iter->second.SourceAssetPath.empty();
					display = (has_source
						? PathToUtf8(PathFromUtf8(iter->second.SourceAssetPath).filename())
						: std::string("(material instance)")) + "   [Instance]";
					tooltip_path = has_source
						? RELATIVE_PATH(iter->second.SourceAssetPath)
						: std::string("(material instance)");
				}
				else
				{
					/* "覆盖"约定挂的是材质资产；运行时构造的匿名材质没有路径 */
					const std::string& material_path = iter->second.pMaterial->GetPath();
					display = material_path.empty()
						? std::string("(material)")
						: PathToUtf8(PathFromUtf8(material_path).filename());
					tooltip_path = material_path.empty() ? std::string("(material)") : RELATIVE_PATH(material_path);
				}
			}
			else if (slot->pDefault != nullptr && !slot->pDefault->GetPath().empty())
			{
				display = PathToUtf8(PathFromUtf8(slot->pDefault->GetPath()).filename());
				tooltip_path = RELATIVE_PATH(slot->pDefault->GetPath());
			}
			else
			{
				display = "Builtin White";
				tooltip_path = "(builtin white)";
			}

			const float gap = ImGui::GetStyle().ItemInnerSpacing.x;

			const float value_width = ImGuiExt::BeginPropertyRow(slot->Name.c_str());

			/* 行内自算尺寸（X / Reset 按钮高）必须在 Begin 之后取：行高由
			 * BeginPropertyRow 统一压成"字体高"，这里要跟行内实际控件一致 */
			const float frame_height = ImGui::GetFrameHeight();

			/* 右端按钮区：清除（X）+ 实例时的重置 */
			const float reset_width = is_instance
				? ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2.0f
				: 0.0f;
			float button_zone = has_override ? (frame_height + gap) : 0.0f;
			if (is_instance)
				button_zone += reset_width + gap;

			const float target_width = std::max(1.0f, value_width - button_zone);

			DrawRefInputBox("##MaterialRef", display, target_width, nullptr);

			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s\n\nDrag a .mtl material asset here to assign or replace this slot's material",
					tooltip_path.c_str());

			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RESOURCE_BROWSER_ITEM"))
				{
					std::filesystem::path relative_path;
					if (payload->DataSize > 1 && TryPathFromUtf8Payload(
						payload->Data, static_cast<size_t>(payload->DataSize), relative_path))
					{
						std::string extension = PathToUtf8(relative_path.extension());
						std::transform(extension.begin(), extension.end(), extension.begin(),
							[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

						if (extension == ".mtl")
						{
							const std::string absolute_path = PathToUtf8(g_AssetsPath / relative_path);
							auto material = AssetManager::Instance().GetOrLoad<Material>(absolute_path);
							if (material != nullptr)
							{
								MaterialSlotOverride slot_override;
								slot_override.pMaterial = std::move(material);
								component.m_SlotOverrides[slot->Name] = std::move(slot_override);
								changed = true;
							}
						}
					}
				}
				ImGui::EndDragDropTarget();
			}

			if (is_instance)
			{
				ImGui::SameLine(0.0f, gap);
				if (ImGui::Button("Reset", ImVec2(reset_width, frame_height)))
				{
					/* 从来源资产重新克隆（无来源 = 回默认） */
					if (!iter->second.SourceAssetPath.empty())
					{
						auto source = AssetManager::Instance().GetOrLoad<Material>(iter->second.SourceAssetPath);
						if (source != nullptr)
						{
							auto& slot_override = component.m_SlotOverrides[slot->Name];
							slot_override.pMaterial = source->Clone();
							slot_override.IsInstance = true;
							changed = true;
						}
					}
					else
					{
						component.m_SlotOverrides.erase(slot->Name);
						changed = true;
					}
				}

				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Re-clone from the source asset (or fall back to default)");
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
					ImGui::SetTooltip("Clear override / instance, fall back to the model default");
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

			int overridden_count = 0;
			for (const auto& slot : slots)
			{
				const auto iter = component.m_SlotOverrides.find(slot.Name);
				if (iter != component.m_SlotOverrides.end() && iter->second.pMaterial != nullptr)
					++overridden_count;
			}

			ImGui::Spacing();
			ImGui::TextColored(EditorTheme::Token::TextLabel, "Materials");
			ImGui::SameLine();
			/* 统计文案窄窗里必须能折行：裸文本的布局宽度不受裁剪影响，会撑出横向滚动 */
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
			ImGui::TextDisabled("(%d slots, %d overridden)", static_cast<int>(slots.size()), overridden_count);
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

			/* 模型引用输入框：只读展示模型名，拖放 .mesh 直接落在输入框上；
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

			DrawRefInputBox("##ModelRef", model_name, target_width, "Drop .mesh here");

			if (ImGui::IsItemHovered())
			{
				if (model != nullptr)
					ImGui::SetTooltip("%s\n\nDrag a .mesh asset from the Resource Browser to assign or replace the model",
						model_path.c_str());
				else
					ImGui::SetTooltip("Drag a .mesh asset from the Resource Browser to assign the model");
			}

			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RESOURCE_BROWSER_ITEM"))
				{
					std::filesystem::path relative_path;
					if (payload->DataSize > 1 && TryPathFromUtf8Payload(
						payload->Data, static_cast<size_t>(payload->DataSize), relative_path))
					{
						std::string extension = PathToUtf8(relative_path.extension());
						std::transform(extension.begin(), extension.end(), extension.begin(),
							[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

						if (extension == ".mesh")
						{
							const std::string absolute_path = PathToUtf8(g_AssetsPath / relative_path);
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

			/* 材质槽区块：每槽一行 + 当前材质属性（可就地编辑） */
			if (model != nullptr)
				changed |= DrawMaterialSlots(component, *model);

			return changed;
		}
	}

	void RegisterModelComponent()
	{
		MakeRegistrar<ModelComponent>("Model")
			.SaveExtra(&SaveModelPath)
			.LoadExtra(&LoadModelPath)
			.CustomDraw(&DrawModelBlock)
			.Register();
	}
}
