#pragma once
/* 轴字母（X / Y / Z / W）的矢量笔画 —— 视图指示器和分量重置按钮共用（两处"轴盘语言"逐笔
 * 同源）。用矢量笔画而不是字体：ImFont::RenderText 会把落点对齐到整数逻辑像素，拖动时字母
 * 逐像素跳；矢量笔画是亚像素落点、跟盘一起平滑移动。 */
#include <imgui.h>

namespace Helios::AxisGlyph
{
	/* 字母盒（宽 / 高）与笔画粗细 */
	inline constexpr float kLetterWidth = 8.0f;
	inline constexpr float kLetterHeight = 9.0f;
	inline constexpr float kLetterStroke = 1.8f;
	/* 收边 / 圆环线宽：轴盘压在别的元素上时，用最深底色的描边把轮廓"切"出来 */
	inline constexpr float kRimStroke = 1.5f;

	/* 单个字母（X / Y / Z / W）：三笔以内的直笔画。坐标全部从盘心派生、
	 * 不经过任何取整 —— "拖动时字和盘一起平滑走"靠的就是这一点。 */
	inline void DrawLetter(ImDrawList* draw_list, char letter, const ImVec2& center, ImU32 ink)
	{
		const float half_w = kLetterWidth * 0.5f;
		const float half_h = kLetterHeight * 0.5f;

		const ImVec2 top_left(center.x - half_w, center.y - half_h);
		const ImVec2 top_right(center.x + half_w, center.y - half_h);
		const ImVec2 bottom_left(center.x - half_w, center.y + half_h);
		const ImVec2 bottom_right(center.x + half_w, center.y + half_h);

		switch (letter)
		{
		case 'X':
			draw_list->AddLine(top_left, bottom_right, ink, kLetterStroke);
			draw_list->AddLine(top_right, bottom_left, ink, kLetterStroke);
			break;
		case 'Y':
		{
			/* 分叉点略高于中线（与字体的 Y 一致），三笔共点 */
			const ImVec2 junction(center.x, center.y - 1.0f);
			draw_list->AddLine(top_left, junction, ink, kLetterStroke);
			draw_list->AddLine(top_right, junction, ink, kLetterStroke);
			draw_list->AddLine(junction, ImVec2(center.x, center.y + half_h), ink, kLetterStroke);
			break;
		}
		case 'W':
		{
			/* 四笔：两个落点在中线两侧的四分之一宽处，中峰顶到接近盒顶
			 * （与字体的 W 一致；峰低了四笔角度会不一致，看着就不像 W） */
			const ImVec2 down_left(center.x - half_w * 0.5f, center.y + half_h);
			const ImVec2 peak(center.x, center.y - half_h * 0.9f);
			const ImVec2 down_right(center.x + half_w * 0.5f, center.y + half_h);
			draw_list->AddLine(top_left, down_left, ink, kLetterStroke);
			draw_list->AddLine(down_left, peak, ink, kLetterStroke);
			draw_list->AddLine(peak, down_right, ink, kLetterStroke);
			draw_list->AddLine(down_right, top_right, ink, kLetterStroke);
			break;
		}
		case 'Z':
		default:
			draw_list->AddLine(top_left, top_right, ink, kLetterStroke);
			draw_list->AddLine(top_right, bottom_left, ink, kLetterStroke);
			draw_list->AddLine(bottom_left, bottom_right, ink, kLetterStroke);
			break;
		}
	}
}
