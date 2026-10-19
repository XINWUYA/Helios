#include "Pch.h"
#include "Helios/Reflection/ComponentRegistration.h"
#include "Helios/ImGui/ImGuiExtensions.h"
#include "Helios/Scene/AnimationClip.h"
#include "Helios/Scene/Components.h"
#include "Helios/Scene/SceneCommon.h"
#include "Helios/Core/AssetManager.h"
#include <tinyxml2.h>
#include <algorithm>
#include <cctype>

namespace Helios
{
	namespace
	{
		void SaveAnimator(tinyxml2::XMLElement* element, const void* component)
		{
			const auto& animator = *static_cast<const AnimatorComponent*>(component);
			if (animator.m_Clip != nullptr && !animator.m_Clip->GetPath().empty())
				element->SetAttribute("Clip", RELATIVE_PATH(animator.m_Clip->GetPath()).c_str());

			element->SetAttribute("Speed", animator.m_Speed);
			element->SetAttribute("Loop", animator.m_Loop);
			element->SetAttribute("AutoPlay", animator.m_AutoPlay);
		}

		void LoadAnimator(const tinyxml2::XMLElement* element, void* component)
		{
			auto& animator = *static_cast<AnimatorComponent*>(component);
			animator.m_Speed = element->FloatAttribute("Speed", 1.0f);
			animator.m_Loop = element->BoolAttribute("Loop", true);
			animator.m_AutoPlay = element->BoolAttribute("AutoPlay", false);

			const char* path = element->Attribute("Clip");
			if (path == nullptr || *path == '\0')
				return;

			/* 属性里存的是相对资产根的形态，入口即转绝对（与模型 / 材质同一规则） */
			animator.m_Clip = AssetManager::Instance().GetOrLoad<AnimationClip>(ABSOLUTE_PATH(path));
			if (animator.m_Clip == nullptr)
				CORE_LOG_WARN("Failed to load animation clip: {}.", path);
		}

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
						std::string extension = PathToUtf8(relative_path.extension());
						std::transform(extension.begin(), extension.end(), extension.begin(),
							[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

						if (extension == ".anim")
						{
							const std::string absolute_path = PathToUtf8(g_AssetsPath / relative_path);
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

	void RegisterAnimatorComponent()
	{
		MakeRegistrar<AnimatorComponent>("Animator")
			.SaveExtra(&SaveAnimator)
			.LoadExtra(&LoadAnimator)
			.CustomDraw(&DrawAnimatorBlock)
			.Register();
	}
}
