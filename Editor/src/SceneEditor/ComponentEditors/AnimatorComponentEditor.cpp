#include "Pch.h"
#include "ComponentEditors.h"
#include "Common/TextMatch.h"
#include "Helios/Core/AssetManager.h"
#include "ImGui/ImGuiExtensions.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include "Helios/Scene/AnimationClip.h"
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
		/* 资源引用输入框：只读展示 clip 文件名，拖入 .anim 即挂上 */
		void DrawClipRow(AnimatorComponent& component, bool& changed)
		{
			const float value_width = ImGuiExt::BeginPropertyRow("Clip");

			std::string display = "None";
			std::string tooltip = "(no clip)";
			if (component.m_Clip != nullptr)
			{
				display = PathToUtf8(PathFromUtf8(component.m_Clip->GetPath()).filename());
				tooltip = RELATIVE_PATH(component.m_Clip->GetPath());
			}

			char buffer[256] = {};
			std::strncpy(buffer, display.c_str(), sizeof(buffer) - 1);
			ImGui::SetNextItemWidth(value_width);
			ImGui::InputTextWithHint("##AnimatorClip", "Drop .anim here", buffer, sizeof(buffer),
				ImGuiInputTextFlags_ReadOnly);

			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s\n\nDrag an .anim clip here to play it", tooltip.c_str());

			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RESOURCE_BROWSER_ITEM"))
				{
					std::filesystem::path relative_path;
					if (payload->DataSize > 1 && TryPathFromUtf8Payload(
						payload->Data, static_cast<size_t>(payload->DataSize), relative_path))
					{
						const std::string extension = LowercaseExtension(relative_path);

						if (extension == ".anim")
						{
							const std::string absolute_path = PathToUtf8(ResolveAssetPath(relative_path));
							auto clip = AssetManager::Instance().GetOrLoad<AnimationClip>(absolute_path);
							if (clip != nullptr)
							{
								component.m_Clip = std::move(clip);
								component.m_Loop = component.m_Clip->IsLoop();
								component.m_Playing = false;
								component.m_Scrubbing = false;
								component.m_Time = 0.0f;
								changed = true;
							}
						}
					}
				}
				ImGui::EndDragDropTarget();
			}

			ImGuiExt::EndPropertyRow();
		}

		/* 播放控制：播放 / 停止 + 当前时间。播放状态是运行期的，不进撤销与存盘。 */
		void DrawPlaybackRow(AnimatorComponent& component)
		{
			ImGuiExt::BeginPropertyRow("Playback");

			const float button_width = ImGui::CalcTextSize("Stop").x + ImGui::GetStyle().FramePadding.x * 4.0f;
			ImGui::BeginDisabled(component.m_Clip == nullptr);
			if (ImGui::Button(component.m_Playing ? "Stop" : "Play", ImVec2(button_width, 0.0f)))
			{
				component.m_Playing = !component.m_Playing;
				component.m_Scrubbing = false;
				if (component.m_Playing)
					component.m_Time = 0.0f;
			}
			ImGui::EndDisabled();

			ImGui::SameLine();
			const float duration = (component.m_Clip != nullptr) ? component.m_Clip->GetDuration() : 0.0f;
			ImGui::TextDisabled("%.2f / %.2f s", component.m_Time, duration);

			ImGuiExt::EndPropertyRow();
		}

		void DrawOptionsRow(AnimatorComponent& component, bool& changed)
		{
			ImGuiExt::BeginPropertyRow("Options");

			if (ImGui::Checkbox("Loop", &component.m_Loop))
				changed = true;
			ImGui::SameLine();
			if (ImGui::Checkbox("Auto Play", &component.m_AutoPlay))
				changed = true;

			ImGuiExt::EndPropertyRow();
		}

		void DrawSpeedRow(AnimatorComponent& component, bool& changed)
		{
			const float value_width = ImGuiExt::BeginPropertyRow("Speed");

			const float before = component.m_Speed;
			ImGui::SetNextItemWidth(value_width);
			ImGui::DragFloat("##AnimatorSpeed", &component.m_Speed,
				AnimatorComponent::kSpeedStep, AnimatorComponent::kSpeedMin, AnimatorComponent::kSpeedMax, "%.2fx");
			if (component.m_Speed != before)
				changed = true;

			ImGuiExt::EndPropertyRow();
		}

		bool DrawAnimatorBlock(void* raw)
		{
			auto& component = *static_cast<AnimatorComponent*>(raw);
			bool changed = false;

			DrawClipRow(component, changed);
			DrawPlaybackRow(component);
			DrawOptionsRow(component, changed);
			DrawSpeedRow(component, changed);

			/* 只有作者数据（clip / 循环 / 自动播放 / 速度）进撤销；播放与时间是运行态 */
			return changed;
		}
	}

	void RegisterAnimatorComponentEditor()
	{
		ComponentRegistry::Instance().SetCustomDraw(typeid(AnimatorComponent), &DrawAnimatorBlock);
	}
}
