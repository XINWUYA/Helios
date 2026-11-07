#pragma once
#include <functional>

#include <imgui.h>

#include "UI/EditorTheme.h"

namespace Helios
{
	namespace Widgets
	{
		/* 曲线配色的语义（与图例同源）：Frame = 强调、CPU = Z 轴蓝、GPU = Y 轴绿 */
		inline const ImVec4 kFrameLineColor = EditorTheme::Token::Accent;
		inline const ImVec4 kCpuLineColor = EditorTheme::Token::AxisZ;
		inline const ImVec4 kGpuLineColor = EditorTheme::Token::AxisY;

		/* 画一条可能中断的折线：有效点连续段各自成笔，断开处不连线 */
		void DrawBrokenSeries(ImDrawList* draw_list, uint32_t count,
			const std::function<bool(uint32_t)>& valid_at,
			const std::function<ImVec2(uint32_t)>& point_at,
			ImU32 color, float thickness);

		/* 帧号滑条（录制回放的选帧）：点击跳转 / 拖动选帧，返回是否被改动 */
		bool FrameSlider(const char* id, int& value, int v_min, int v_max);
	}
}
