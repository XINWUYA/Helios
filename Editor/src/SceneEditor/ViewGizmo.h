#pragma once
/* 视图指示器（视口右上角六轴盘）的几何与绘制 —— 跟交互解耦：位置投影 / 深度排序 / 命中 /
 * 逐盘绘制在这里，交互（点击切视角、拖拽轨道）留在 SceneEditorLayer；reflect_check 预览
 * 程序复用同一份绘制代码出图。
 * 轴盘语言：正轴 = 实心盘 + 字母、负轴 = 空心环 + 字母；绘制按远→近、近的压盖。 */

#include <imgui.h>
#include <glm/glm.hpp>

#include <algorithm>

#include "Helios/ImGui/EditorTheme.h"
#include "Helios/ImGui/AxisGlyph.h"

namespace Helios::ViewGizmo
{
	inline constexpr int32_t kDiscCount = 6;
	/* 轴盘半径：命中与绘制共用同一值（悬停外扩只影响观感，不改变命中） */
	inline constexpr float kDiscRadius = 10.5f;
	/* 悬停 / 按下：轴色亮一档 + 外扩一圈（与主题控件"提亮一档"的状态语言一致） */
	inline constexpr float kHighlightGrow = 2.0f;

	/* 六个轴盘方向：+X / -X / +Y / -Y / +Z / -Z */
	inline const glm::vec3 kDiscDirections[kDiscCount] = {
		{  1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f },
		{  0.0f, 1.0f, 0.0f }, {  0.0f, -1.0f, 0.0f },
		{  0.0f, 0.0f, 1.0f }, {  0.0f, 0.0f, -1.0f } };

	struct Disc
	{
		int32_t Index;
		ImVec2 Position;
		float Depth;	/* 视线空间 z：越大越靠近观察者 */
	};

	/* 三个轴的配色与字母：与属性面板分量按钮同源（EditorTheme::Token 单一数据源） */
	struct AxisStyle
	{
		const char* Letter;
		ImVec4 Base;
		ImVec4 Hover;
	};

	inline const AxisStyle& AxisStyleOf(int32_t axis)
	{
		static const AxisStyle kAxes[3] = {
			{ "X", EditorTheme::Token::AxisX, EditorTheme::Token::AxisXHover },
			{ "Y", EditorTheme::Token::AxisY, EditorTheme::Token::AxisYHover },
			{ "Z", EditorTheme::Token::AxisZ, EditorTheme::Token::AxisZHover } };
		return kAxes[std::clamp(axis, 0, 2)];
	}

	/* 六个轴盘：世界轴方向经视线旋转（视图矩阵左上 3x3）投影到屏幕的横 / 竖分量；
	 * 结果按深度升序 = 绘制顺序（远者先画）与命中优先级（从近到远取第一个）。
	 * 轴与视线重合时两盘叠在中心，近者在上。 */
	inline void ComputeDiscs(const glm::mat3& view_rotation, const ImVec2& center, float orbit_radius,
		Disc out[kDiscCount])
	{
		for (int32_t i = 0; i < kDiscCount; ++i)
		{
			const glm::vec3 view_dir = view_rotation * kDiscDirections[i];
			out[i] = { i,
				ImVec2(center.x + view_dir.x * orbit_radius, center.y - view_dir.y * orbit_radius),
				view_dir.z };
		}
		std::sort(out, out + kDiscCount, [](const Disc& left, const Disc& right)
			{ return left.Depth < right.Depth; });
	}

	/* 命中轴盘：从近到远找第一个（近者优先），没命中返回 -1 */
	inline int32_t HitTest(const Disc discs[kDiscCount], const ImVec2& point)
	{
		for (int32_t i = kDiscCount - 1; i >= 0; --i)
		{
			const float dx = point.x - discs[i].Position.x;
			const float dy = point.y - discs[i].Position.y;
			if (dx * dx + dy * dy <= kDiscRadius * kDiscRadius)
				return discs[i].Index;
		}
		return -1;
	}

	/* 逐盘绘制（远→近）。正轴实心、负轴空心 —— 正负不再靠明暗区分；悬停 / 按下 = 轴色悬停档 +
	 * 外扩一圈。 */
	inline void Draw(ImDrawList* draw_list, const Disc discs[kDiscCount], int32_t hovered_index, int32_t pressed_index)
	{
		/* 收边 / 垫边：轴盘压在别的元素（含另一个轴盘）上时，用最深底色的描边把轮廓"切"出来 */
		const ImU32 separator = ImGui::GetColorU32(EditorTheme::WithAlpha(EditorTheme::Token::Neutral0, 0.55f));

		for (int32_t i = 0; i < kDiscCount; ++i)
		{
			const Disc& disc = discs[i];

			const AxisStyle& axis = AxisStyleOf(disc.Index / 2);
			const bool positive = (disc.Index % 2) == 0;
			const bool highlighted = (disc.Index == hovered_index) || (disc.Index == pressed_index);

			const ImVec4& color = highlighted ? axis.Hover : axis.Base;
			const float radius = kDiscRadius + (highlighted ? kHighlightGrow : 0.0f);

			if (positive)
			{
				draw_list->AddCircleFilled(disc.Position, radius, ImGui::GetColorU32(color));
				draw_list->AddCircle(disc.Position, radius, separator, 0, AxisGlyph::kRimStroke);
			}
			else
			{
				/* 空心环：先垫一圈底色描边再画本色环 —— 环压在同色实心盘上也分得开 */
				draw_list->AddCircle(disc.Position, radius, separator, 0, AxisGlyph::kRimStroke + 2.0f);
				draw_list->AddCircle(disc.Position, radius, ImGui::GetColorU32(color), 0, AxisGlyph::kRimStroke);
			}

			/* 字母：实心盘用石墨墨色（对三个轴色的对比度都高于浅色字），空心盘用轴色 */
			const ImU32 ink = positive ? ImGui::GetColorU32(EditorTheme::Token::Neutral0)
				: ImGui::GetColorU32(color);
			AxisGlyph::DrawLetter(draw_list, axis.Letter[0], disc.Position, ink);
		}
	}
}
