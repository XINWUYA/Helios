#include "Pch.h"
#include "PanelRegistry.h"
#include <array>
#include <cstring>
#include <imgui.h>

namespace Helios::Panel
{
	namespace
	{
		constexpr size_t kPanelCount = sizeof(kPanels) / sizeof(kPanels[0]);

		std::array<bool, kPanelCount> MakeDefaultFlags()
		{
			std::array<bool, kPanelCount> flags{};
			for (size_t index = 0; index < kPanelCount; ++index)
				flags[index] = kPanels[index].DefaultVisible;
			return flags;
		}

		std::array<bool, kPanelCount> s_Visible = MakeDefaultFlags();
	}

	bool& VisibleFlag(const char* id)
	{
		for (size_t index = 0; index < kPanelCount; ++index)
		{
			if (kPanels[index].Id == id || std::strcmp(kPanels[index].Id, id) == 0)
				return s_Visible[index];
		}

		/* 没登记的窗口当常驻处理：给一份恒为 true 的开关，改了也不影响别人 */
		static bool s_Unregistered = true;
		s_Unregistered = true;
		return s_Unregistered;
	}

	void DrawToggleMenuItems(Menu menu)
	{
		for (size_t index = 0; index < kPanelCount; ++index)
		{
			const Desc& desc = kPanels[index];
			if (desc.ToggleMenu != menu || desc.MenuLabel == nullptr)
				continue;

			ImGui::MenuItem(desc.MenuLabel, nullptr, &s_Visible[index]);
			if (desc.Tooltip != nullptr && ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", desc.Tooltip);
		}
	}
}
