#include "Pch.h"

#include "ImGuiExtensions.h"
#include <imgui_internal.h>
#include "Helios/Application/AssetManager.h"
#include "Helios/Common/Math.h"
#include "Helios/ImGui/AxisGlyph.h"
#include "Helios/VirtualDevice/DeviceTexture.h"
#include "Helios/Scene/SceneCommon.h"

namespace Helios::ImGuiExt
{
	static inline ImVec2 operator+(const ImVec2& lft, const ImVec2& rht)
	{
		return { lft.x + rht.x, lft.y + rht.y };
	}

	static inline ImVec2 operator-(const ImVec2& lft, const ImVec2& rht)
	{
		return { lft.x - rht.x, lft.y - rht.y };
	}

	inline float Cross(const ImVec2& lft, const ImVec2& rht)
	{
		return lft.x * rht.y - lft.y * rht.x;
	}

	glm::quat QuatFromAxisAngle(const glm::vec3& axis, float angle)
	{
		auto normalized_axis = glm::normalize(axis);
		float sin_half_angle = std::sin(angle * 0.5f);
		return {
			std::cos(angle * 0.5f),
			normalized_axis.x * sin_half_angle,
			normalized_axis.y * sin_half_angle,
			normalized_axis.z * sin_half_angle
		};
	}

	class ArrowWidget {
	public:
		explicit ArrowWidget(const glm::vec3& direction);
		bool Draw();

		[[nodiscard]]
		glm::vec3 GetDirection() const;

	private:
		enum EArrowParts
		{
			ARROW_CONE,
			ARROW_CONE_CAP,
			ARROW_CYL,
			ARROW_CYL_CAP
		};

		static void CreateArrow();
		void DrawTriangles(ImDrawList* draw_list, const ImVec2& offset, const ImVector<ImVec2>& triProj, const ImVector<ImU32>& colLight, int numVertices);
		static float QuatD(float w, float h) { return std::min(std::abs(w), std::abs(h)) - 4.0f; }
		static float QuatPX(float x, float w, float h) { return (x * 0.5f * QuatD(w, h) + w * 0.5f + 0.5f); }
		static float QuatPY(float y, float w, float h) { return (-y * 0.5f * QuatD(w, h) + h * 0.5f - 0.5f); }
		static float QuatIX(int x, float w, float h) { return (2.0f * x - w - 1.0f) / QuatD(w, h); }
		static float QuatIY(int y, float w, float h) { return (-2.0f * y + h - 1.0f) / QuatD(w, h); }
		static void QuatFromDirection(glm::quat& quat, const glm::vec3& dir);
		static ImU32 BlendColor(ImU32 c1, ImU32 c2, float t);

		const ImU32 mDirColor = 0xff00ffff;
		const int mWidgetSize = 100;
		glm::quat mDirectionQuat{};
	};

	static ImVector<glm::vec3> s_ArrowTri[4];
	static ImVector<glm::vec3> s_ArrowNorm[4];
	static ImVector<ImVec2> s_ArrowTriProj[4];
	static ImVector<ImU32> s_ArrowColLight[4];

	ArrowWidget::ArrowWidget(const glm::vec3& direction)
	{
		QuatFromDirection(mDirectionQuat, direction);
	}

	bool ArrowWidget::Draw()
	{
		ImGuiStyle& style = ImGui::GetStyle();
		ImDrawList* draw_list = ImGui::GetWindowDrawList();

		if (s_ArrowTri[0].empty()) {
			CreateArrow();
		}

		bool value_changed = false;

		ImVec2 orient_pos = ImGui::GetCursorScreenPos();

		float sv_orient_size = std::min(ImGui::CalcItemWidth(), float(mWidgetSize));
		float w = sv_orient_size;
		float h = sv_orient_size;

		// We want to generate quaternion rotations relative to the quaternion in the down press state.
		// This gives us cleaner control over rotation (it feels better)
		static glm::quat origin_quat;
		static glm::vec3 old_coord;
		bool highlighted = false;
		ImGui::InvisibleButton("widget", ImVec2(sv_orient_size, sv_orient_size));
		if (ImGui::IsItemActive()) {
			highlighted = true;
			ImVec2 mouse = ImGui::GetMousePos() - orient_pos;
			if (ImGui::IsMouseClicked(0)) {
				origin_quat = mDirectionQuat;
				old_coord = glm::vec3(QuatIX((int)mouse.x, w, h), QuatIY((int)mouse.y, w, h), 1.0f);
			}
			else if (ImGui::IsMouseDragging(0)) {
				glm::vec3 coord(QuatIX((int)mouse.x, w, h), QuatIY((int)mouse.y, w, h), 1.0f);
				glm::vec3 pVec = coord;
				glm::vec3 oVec = old_coord;
				coord.z = 0.0f;
				float n0 = glm::length(oVec);
				float n1 = glm::length(pVec);
				if (n0 > FLT_EPSILON && n1 > FLT_EPSILON) {
					glm::vec3 v0 = oVec / n0;
					glm::vec3 v1 = pVec / n1;
					glm::vec3 axis = glm::cross(v0, v1);
					float sa = glm::length(axis);
					float ca = glm::dot(v0, v1);
					float angle = atan2(sa, ca);
					if (coord.x * coord.x + coord.y * coord.y > 1.0) {
						angle *= 1.0f + 1.5f * (glm::length(coord) - 1.0f);
					}
					glm::quat qrot, qres, qorig;
					qrot = QuatFromAxisAngle(axis, angle);
					float nqorig = sqrt(origin_quat.x * origin_quat.x + origin_quat.y * origin_quat.y +
						origin_quat.z * origin_quat.z + origin_quat.w * origin_quat.w);
					if (std::abs(nqorig) > FLT_EPSILON * FLT_EPSILON) {
						qorig = origin_quat / nqorig;
						qres = qrot * qorig;
						mDirectionQuat = qres;
					}
					else {
						mDirectionQuat = qrot;
					}
					value_changed = true;
				}
			}
			draw_list->AddRectFilled(orient_pos, orient_pos + ImVec2(sv_orient_size, sv_orient_size),
				ImColor(style.Colors[ImGuiCol_FrameBgActive]), style.FrameRounding);
		}
		else {
			ImColor color(ImGui::IsItemHovered() ? style.Colors[ImGuiCol_FrameBgHovered] :
				style.Colors[ImGuiCol_FrameBg]);
			draw_list->AddRectFilled(orient_pos, orient_pos + ImVec2(sv_orient_size, sv_orient_size),
				color, style.FrameRounding);
		}

		ImVec2 inner_pos = orient_pos;
		glm::quat quat = normalize(mDirectionQuat);
		ImColor alpha(1.0f, 1.0f, 1.0f, highlighted ? 1.0f : 0.75f);
		glm::vec3 arrowDir = quat * glm::vec3(1, 0, 0);

		for (int k = 0; k < 4; ++k) {
			int j = (arrowDir.z > 0) ? 3 - k : k;
			ASSERT(s_ArrowTriProj[j].size() == (s_ArrowTri[j].size()) &&
				s_ArrowColLight[j].size() == s_ArrowTri[j].size() &&
				s_ArrowNorm[j].size() == s_ArrowTri[j].size());
			size_t ntri = s_ArrowTri[j].size();
			for (int i = 0; i < ntri; ++i) {
				glm::vec3 coord = s_ArrowTri[j][i];
				glm::vec3 norm = s_ArrowNorm[j][i];
				if (coord.x > 0) {
					coord.x = 2.5f * coord.x - 2.0f;
				}
				else {
					coord.x += 0.2f;
				}
				coord.y *= 1.5f;
				coord.z *= 1.5f;
				coord = quat * coord;
				norm = quat * norm;
				s_ArrowTriProj[j][i] = ImVec2(QuatPX(coord.x, w, h), QuatPY(coord.y, w, h));
				ImU32 col = (mDirColor | 0xff000000) & alpha;
				s_ArrowColLight[j][i] = BlendColor(0xff000000, col, std::abs(glm::clamp(norm.z, -1.0f, 1.0f)));
			}
			DrawTriangles(draw_list, inner_pos, s_ArrowTriProj[j], s_ArrowColLight[j], ntri);
		}

		return value_changed;
	}

	glm::vec3 ArrowWidget::GetDirection() const
	{
		glm::vec3 d = mDirectionQuat * glm::vec3(1, 0, 0);
		return d / length(d);
	}

	void ArrowWidget::CreateArrow()
	{
		constexpr int SUBDIV = 15;
		constexpr float CYL_RADIUS = 0.06f;
		constexpr float CONE_RADIUS = 0.16f;
		constexpr float CONE_LENGTH = 0.20f;
		constexpr float ARROW_BGN = -1.1f;
		constexpr float ARROW_END = 1.15f;

		for (int i = 0; i < 4; ++i) {
			s_ArrowTri[i].clear();
			s_ArrowNorm[i].clear();
		}

		float x0, x1, y0, y1, z0, z1, a0, a1, nx, nn;
		for (int i = 0; i < SUBDIV; ++i) {
			a0 = 2.0f * PI * (float(i)) / SUBDIV;
			a1 = 2.0f * PI * (float(i + 1)) / SUBDIV;
			x0 = ARROW_BGN;
			x1 = ARROW_END - CONE_LENGTH;
			y0 = cosf(a0);
			z0 = sinf(a0);
			y1 = cosf(a1);
			z1 = sinf(a1);
			s_ArrowTri[ARROW_CYL].push_back(glm::vec3(x1, CYL_RADIUS * y0, CYL_RADIUS * z0));
			s_ArrowTri[ARROW_CYL].push_back(glm::vec3(x0, CYL_RADIUS * y0, CYL_RADIUS * z0));
			s_ArrowTri[ARROW_CYL].push_back(glm::vec3(x0, CYL_RADIUS * y1, CYL_RADIUS * z1));
			s_ArrowTri[ARROW_CYL].push_back(glm::vec3(x1, CYL_RADIUS * y0, CYL_RADIUS * z0));
			s_ArrowTri[ARROW_CYL].push_back(glm::vec3(x0, CYL_RADIUS * y1, CYL_RADIUS * z1));
			s_ArrowTri[ARROW_CYL].push_back(glm::vec3(x1, CYL_RADIUS * y1, CYL_RADIUS * z1));
			s_ArrowNorm[ARROW_CYL].push_back(glm::vec3(0, y0, z0));
			s_ArrowNorm[ARROW_CYL].push_back(glm::vec3(0, y0, z0));
			s_ArrowNorm[ARROW_CYL].push_back(glm::vec3(0, y1, z1));
			s_ArrowNorm[ARROW_CYL].push_back(glm::vec3(0, y0, z0));
			s_ArrowNorm[ARROW_CYL].push_back(glm::vec3(0, y1, z1));
			s_ArrowNorm[ARROW_CYL].push_back(glm::vec3(0, y1, z1));
			s_ArrowTri[ARROW_CYL_CAP].push_back(glm::vec3(x0, 0, 0));
			s_ArrowTri[ARROW_CYL_CAP].push_back(glm::vec3(x0, CYL_RADIUS * y1, CYL_RADIUS * z1));
			s_ArrowTri[ARROW_CYL_CAP].push_back(glm::vec3(x0, CYL_RADIUS * y0, CYL_RADIUS * z0));
			s_ArrowNorm[ARROW_CYL_CAP].push_back(glm::vec3(-1, 0, 0));
			s_ArrowNorm[ARROW_CYL_CAP].push_back(glm::vec3(-1, 0, 0));
			s_ArrowNorm[ARROW_CYL_CAP].push_back(glm::vec3(-1, 0, 0));
			x0 = ARROW_END - CONE_LENGTH;
			x1 = ARROW_END;
			nx = CONE_RADIUS / (x1 - x0);
			nn = 1.0f / sqrtf(nx * nx + 1);
			s_ArrowTri[ARROW_CONE].push_back(glm::vec3(x1, 0, 0));
			s_ArrowTri[ARROW_CONE].push_back(glm::vec3(x0, CONE_RADIUS * y0, CONE_RADIUS * z0));
			s_ArrowTri[ARROW_CONE].push_back(glm::vec3(x0, CONE_RADIUS * y1, CONE_RADIUS * z1));
			s_ArrowTri[ARROW_CONE].push_back(glm::vec3(x1, 0, 0));
			s_ArrowTri[ARROW_CONE].push_back(glm::vec3(x0, CONE_RADIUS * y1, CONE_RADIUS * z1));
			s_ArrowTri[ARROW_CONE].push_back(glm::vec3(x1, 0, 0));
			s_ArrowNorm[ARROW_CONE].push_back(glm::vec3(nn * nx, nn * y0, nn * z0));
			s_ArrowNorm[ARROW_CONE].push_back(glm::vec3(nn * nx, nn * y0, nn * z0));
			s_ArrowNorm[ARROW_CONE].push_back(glm::vec3(nn * nx, nn * y1, nn * z1));
			s_ArrowNorm[ARROW_CONE].push_back(glm::vec3(nn * nx, nn * y0, nn * z0));
			s_ArrowNorm[ARROW_CONE].push_back(glm::vec3(nn * nx, nn * y1, nn * z1));
			s_ArrowNorm[ARROW_CONE].push_back(glm::vec3(nn * nx, nn * y1, nn * z1));
			s_ArrowTri[ARROW_CONE_CAP].push_back(glm::vec3(x0, 0, 0));
			s_ArrowTri[ARROW_CONE_CAP].push_back(glm::vec3(x0, CONE_RADIUS * y1, CONE_RADIUS * z1));
			s_ArrowTri[ARROW_CONE_CAP].push_back(glm::vec3(x0, CONE_RADIUS * y0, CONE_RADIUS * z0));
			s_ArrowNorm[ARROW_CONE_CAP].push_back(glm::vec3(-1, 0, 0));
			s_ArrowNorm[ARROW_CONE_CAP].push_back(glm::vec3(-1, 0, 0));
			s_ArrowNorm[ARROW_CONE_CAP].push_back(glm::vec3(-1, 0, 0));
		}

		for (int i = 0; i < 4; ++i) {
			s_ArrowTriProj[i].clear();
			s_ArrowTriProj[i].resize(s_ArrowTri[i].size());
			s_ArrowColLight[i].clear();
			s_ArrowColLight[i].resize(s_ArrowTri[i].size());
		}
	}

	void ArrowWidget::DrawTriangles(ImDrawList* draw_list, const ImVec2& offset, const ImVector<ImVec2>& triProj, const ImVector<ImU32>& colLight, int numVertices)
	{
		const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
		ASSERT(numVertices % 3 == 0);
		draw_list->PrimReserve(numVertices, numVertices);
		for (int ii = 0; ii < numVertices / 3; ii++) {
			ImVec2 v1 = offset + triProj[ii * 3];
			ImVec2 v2 = offset + triProj[ii * 3 + 1];
			ImVec2 v3 = offset + triProj[ii * 3 + 2];

			// 2D cross product to do culling
			ImVec2 d1 = v2 - v1;
			ImVec2 d2 = v3 - v1;
			float c = Cross(d1, d2);
			if (c > 0.0f) {
				v2 = v1;
				v3 = v1;
			}

			draw_list->PrimWriteIdx(ImDrawIdx(draw_list->_VtxCurrentIdx));
			draw_list->PrimWriteIdx(ImDrawIdx(draw_list->_VtxCurrentIdx + 1));
			draw_list->PrimWriteIdx(ImDrawIdx(draw_list->_VtxCurrentIdx + 2));
			draw_list->PrimWriteVtx(v1, uv, colLight[ii * 3]);
			draw_list->PrimWriteVtx(v2, uv, colLight[ii * 3 + 1]);
			draw_list->PrimWriteVtx(v3, uv, colLight[ii * 3 + 2]);
		}
	}

	void ArrowWidget::QuatFromDirection(glm::quat& quat, const glm::vec3& dir)
	{
		const float length = glm::length(dir);
		if (length < FLT_EPSILON * FLT_EPSILON)
		{
			quat = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			return;
		}

		glm::vec3 rot_axis = { 0, -dir.z, dir.y };
		if (glm::dot(rot_axis, rot_axis) < FLT_EPSILON * FLT_EPSILON) 
		{
			rot_axis.x = rot_axis.y = 0; rot_axis.z = 1;
		}
		const float rot_angle = acos(glm::clamp(dir.x / length, -1.0f, 1.0f));
		quat = QuatFromAxisAngle(rot_axis, rot_angle);
	}

	ImU32 ArrowWidget::BlendColor(ImU32 c1, ImU32 c2, float t)
	{
		ImColor color1(c1);
		ImColor color2(c2);
		float invt = 1.0f - t;
		color1 = ImColor((color1.Value.x * invt) + (color2.Value.x * t),
			(color1.Value.y * invt) + (color2.Value.y * t),
			(color1.Value.z * invt) + (color2.Value.z * t),
			(color1.Value.w * invt) + (color2.Value.w * t));
		return color1;
	}

	/* ==================== 属性行的统一布局 ====================
	 * 一行 = 左列标签（弱化色、定宽）+ 右列控件（占满剩余宽度）。不用 ImGui::Columns：
	 * 卡片背景靠 draw list 通道延后绘制，而 Columns 会把通道占死（俩没法共存）。
	 * 改用 SameLine(绝对偏移) 手工分列，效果一样、还不占通道。 */

	float BeginPropertyRow(const char* label, float label_width, bool frame_aligned)
	{
		PROFILE_FUNCTION();

		const ImGuiStyle& style = ImGui::GetStyle();

		ImGui::PushID(label);

		/* 行高 = 字体高 + 2×PropertyRowPadY：整行推 FramePadding.y，行内控件（按钮 /
		 * 拖拽框 / 输入框 / 复选框）一律按 GetFrameHeight() 定高；收尾弹回。
		 * 行内自算尺寸必须在 Begin 之后调 GetFrameHeight()，才跟这一档一致。 */
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
			ImVec2(style.FramePadding.x, EditorTheme::Token::PropertyRowPadY));

		/* 文本和控件文字同线：AlignTextToFramePadding 把文本基线下移一档、行高也撑起来 —— 纯文本行
		 * 和带控件行等高、行内内容竖直居中。frame_aligned 参数留着是为了稳住调用点（就是默认行为）。 */
		(void)frame_aligned;
		ImGui::AlignTextToFramePadding();

		const float start_x = ImGui::GetCursorPosX();
		const float available = ImGui::GetContentRegionAvail().x;

		/* 标签列宽取"设计宽度"和"实际文本所需"里更大的那个：正常都等于设计宽度（各卡左边界对齐）；
		 * 超长字段名就撑宽自己那一行；面板窄到挤不下时按列宽裁剪，不画到控件上。 */
		const float label_col = ImMin(
			ImMax(label_width, ImGui::CalcTextSize(label).x + style.ItemInnerSpacing.x),
			available * 0.6f);

		const ImVec2 label_min = ImGui::GetCursorScreenPos();
		ImGui::PushClipRect(label_min, ImVec2(label_min.x + label_col, FLT_MAX), true);
		ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextLabel);
		ImGui::TextUnformatted(label);
		ImGui::PopStyleColor();
		ImGui::PopClipRect();

		const float value_col = ImMax(available - label_col, 1.0f);
		ImGui::SameLine(start_x + label_col, 0.0f);

		return value_col;
	}

	void EndPropertyRow()
	{
		ImGui::PopStyleVar(); /* 行高那一档（与 BeginPropertyRow 的 Push 配对） */
		ImGui::PopID();
	}

	namespace
	{
		/* 分量色表：与多分量行的分量顺序一一对应（X / Y / Z / W） */
		struct AxisStyle
		{
			const char*  Letter;
			const char*  Id;
			ImVec4       Base;
			ImVec4       Hover;
		};

		const AxisStyle& Axis(int index)
		{
			static const AxisStyle kAxes[4] = {
				{ "X", "##X", EditorTheme::Token::AxisX, EditorTheme::Token::AxisXHover },
				{ "Y", "##Y", EditorTheme::Token::AxisY, EditorTheme::Token::AxisYHover },
				{ "Z", "##Z", EditorTheme::Token::AxisZ, EditorTheme::Token::AxisZHover },
				/* 四分量（如权重）没有约定俗成的颜色，用中性灰 */
				{ "W", "##W", EditorTheme::Token::Neutral7, EditorTheme::Token::Neutral8 },
			};
			return kAxes[ImClamp(index, 0, 3)];
		}

		/* 分量重置按钮：轴色实心圆角色块（悬停亮 / 按下暗，跟 Button 三档一致），字母用视图指示器
		 * 同款矢量笔画、白色墨。每枚按钮带起一个「按钮 + 值」小组（左端圆角、右端直角）；点一下把
		 * 该分量恢复成默认值。命中格 (高+3)×高，行内布局不变。 */
		bool DrawAxisButton(const AxisStyle& axis, float height)
		{
			/* 命中 id 用 axis.Letter（"X"），绝不能复用 axis.Id（"##X"）—— 那是同行拖拽框的 id。
			 * 两个控件共 id 时：拖拽框一激活就把 ActiveIdMouseButton 重置成 -1，下一帧本按钮的 ButtonBehavior
			 * 在 Mouse 分支读到 -1、直接触发 IM_ASSERT 崩（回归见 vec_row_input_check）。 */
			const bool clicked = ImGui::InvisibleButton(axis.Letter, ImVec2(height + 3.0f, height));

			const bool hovered = ImGui::IsItemHovered();
			const bool active = ImGui::IsItemActive();
			if (hovered)
				ImGui::SetTooltip("Reset %s", axis.Letter);

			const ImVec4& background = active ? EditorTheme::Token::Neutral6
				: (hovered ? axis.Hover : axis.Base);
			const ImVec2 min = ImGui::GetItemRectMin();
			const ImVec2 max = ImGui::GetItemRectMax();

			ImDrawList* draw_list = ImGui::GetWindowDrawList();
			draw_list->AddRectFilled(min, max, ImGui::GetColorU32(background),
				ImGui::GetStyle().FrameRounding, ImDrawFlags_RoundCornersLeft);
			AxisGlyph::DrawLetter(draw_list, axis.Letter[0],
				ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f),
				ImGui::GetColorU32(ImGuiCol_Text));

			return clicked;
		}

		/* 把刚画完的 frame 控件左缘的两个圆角补成直角（跟左边的轴按钮拼成一体）：按当前状态补填充
		 * 方块 + 直角描边，值框本体走 stock 绘制。注意：坐标必须跟 stock 边框同一笔账（item 矩形内缩
		 * 0.5px；直接用 item 矩形或用 AddLine 会错开 0.5~1px，接缝冒台阶）。 */
		void SquareOffLeftCorners()
		{
			const ImGuiStyle& style = ImGui::GetStyle();
			const float radius = style.FrameRounding;
			if (radius <= 0.0f)
				return;

			const ImU32 fill = ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_FrameBgActive
				: ImGui::IsItemHovered() ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
			const ImU32 border = ImGui::GetColorU32(ImGuiCol_Border);
			const float border_size = style.FrameBorderSize;

			const ImVec2 min = ImGui::GetItemRectMin();
			const ImVec2 max = ImGui::GetItemRectMax();
			/* stock 边框 / 填充的实际路径矩形 = item 矩形内缩 0.5 */
			const ImVec2 frame_min(min.x + 0.5f, min.y + 0.5f);
			const ImVec2 frame_max(max.x - 0.5f, max.y - 0.5f);

			ImDrawList* draw_list = ImGui::GetWindowDrawList();
			draw_list->AddRectFilled(frame_min, ImVec2(frame_min.x + radius, frame_min.y + radius), fill);
			draw_list->AddRectFilled(ImVec2(frame_min.x, frame_max.y - radius),
				ImVec2(frame_min.x + radius, frame_max.y), fill);

			if (border_size > 0.0f)
			{
				/* 左上角：左边线补到顶端 + 上边线补到左端 */
				draw_list->PathLineTo(ImVec2(frame_min.x, frame_min.y + radius));
				draw_list->PathLineTo(frame_min);
				draw_list->PathLineTo(ImVec2(frame_min.x + radius, frame_min.y));
				draw_list->PathStroke(border, 0, border_size);

				/* 左下角：左边线补到底端 + 下边线补到左端 */
				draw_list->PathLineTo(ImVec2(frame_min.x, frame_max.y - radius));
				draw_list->PathLineTo(ImVec2(frame_min.x, frame_max.y));
				draw_list->PathLineTo(ImVec2(frame_min.x + radius, frame_max.y));
				draw_list->PathStroke(border, 0, border_size);
			}
		}

		/* 多分量数值行：每个分量 = 彩色重置按钮 + 拖拽框（「按钮 + 值」小组，组内左圆角 / 相接处直角；
		 * 组间留一档横向间距）。2/3/4 分量共用；uniform_scale：Scale 字段用，拖任一分量时按住 Shift =
		 * 三轴等比。 */
		void DrawVectorRow(const char* label, float* values, int component_count,
		                   float reset_value, float label_width, const char* format,
		                   bool uniform_scale)
		{
			const float value_width = BeginPropertyRow(label, label_width);
			const float frame_height = ImGui::GetFrameHeight();
			const ImGuiStyle& style = ImGui::GetStyle();

			/* 组间距：按钮与值贴合靠行内 ItemSpacing 归零，组与组之间显式给一档 */
			constexpr float kGroupGap = 4.0f;
			const float button_width = frame_height + 3.0f;
			const float box_width = ImMax(12.0f,
				(value_width - static_cast<float>(component_count - 1) * kGroupGap)
					/ static_cast<float>(component_count) - button_width);

			/* 按钮和拖拽框贴合：只把横向间距归零，纵向保留行距 —— ItemSize 会把「行高 + ItemSpacing.y」
			 * 记进光标、EndCard 又按「光标 − ItemSpacing.y」反推卡底；纵向也归零的话，行距和卡底留白就
			 * 按 0 记账（三行贴死、Z 行压上底边框）。 */
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, style.ItemSpacing.y));

			for (int i = 0; i < component_count; ++i)
			{
				const AxisStyle& axis = Axis(i);

				if (i > 0)
					ImGui::SameLine(0.0f, kGroupGap);

				if (DrawAxisButton(axis, frame_height))
					values[i] = reset_value;

				ImGui::SameLine();
				ImGui::SetNextItemWidth(box_width);

				/* Shift 等比：只认"本帧拖动增量"的比值，乘到其余分量上 —— 逐帧比值
				 * 的连乘正好抵消（= 总比值），与帧率无关；本帧起手值为 0 时跳过
				 * （0 没法按比例缩）。 */
				const float before = values[i];
				if (ImGui::DragFloat(axis.Id, &values[i], 0.1f, 0.0f, 0.0f, format))
				{
					if (uniform_scale && ImGui::GetIO().KeyShift && before != 0.0f)
					{
						const float ratio = values[i] / before;
						for (int j = 0; j < component_count; ++j)
						{
							if (j != i)
								values[j] *= ratio;
						}
					}
				}

				if (uniform_scale && ImGui::IsItemHovered() && !ImGui::IsItemActive())
					ImGui::SetTooltip("Drag with Shift to scale all three axes proportionally");

				/* 组内拼接：值框左缘直角（左邻是按钮）；右缘起就是组边界，保留圆角。 */
				SquareOffLeftCorners();
			}

			ImGui::PopStyleVar();
			EndPropertyRow();
		}
	}

	/* 绘制一个普通的文本UI */
	void DrawCommonTextUI(const std::string& label, const std::string& value, float label_width)
	{
		PROFILE_FUNCTION();

		const float value_width = BeginPropertyRow(label.c_str(), label_width);

		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + value_width);
		ImGui::TextUnformatted(value.c_str());
		ImGui::PopTextWrapPos();

		EndPropertyRow();
	}

	/* 绘制一个Color UI */
	void DrawColorUI(const std::string& label, glm::vec4& color, float label_width)
	{
		PROFILE_FUNCTION();

		const float value_width = BeginPropertyRow(label.c_str(), label_width);

		ImGui::SetNextItemWidth(value_width);
		ImGui::ColorEdit4("##Color", glm::value_ptr(color), ImGuiColorEditFlags_AlphaBar);

		EndPropertyRow();
	}

	/* 绘制一个资源引用UI */
	void DrawTextureUI(const std::string& label, SharedPtr<DeviceTexture>& texture, float label_width)
	{
		PROFILE_FUNCTION();

		BeginPropertyRow(label.c_str(), label_width);

		/* 缩略图与文件名分两行：值列通常只有一二百像素，并排会把路径挤成一条竖线 */
		const float value_x = ImGui::GetCursorPosX();

		const auto& show_texture = texture ? texture : TextureAssetManager::Instance().GetOrCreateTexture(ABSOLUTE_PATH("Textures/Default.png"));

		/* Image：ImTextureID 统一存放 DeviceTexture 指针，
		 * 由渲染后端自行解析为硬件句柄（避免 32 位 GetTextureID 截断指针）。 */
		const float thumbnail = 72.0f;
		ImGui::ImageButton((ImTextureID)show_texture.get(), ImVec2(thumbnail, thumbnail),
			ImVec2(0, 1), ImVec2(1, 0), 1, ImVec4(0, 0, 0, 0), ImVec4(1, 1, 1, 1));

		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RESOURCE_BROWSER_ITEM"))
			{
				/* 资源浏览器以 char 字节串（含末尾 NUL）传路径；按 wchar_t 读取会破坏路径。 */
				std::filesystem::path relative_path;
				if (payload->DataSize > 1 && TryPathFromUtf8Payload(
					payload->Data, static_cast<size_t>(payload->DataSize), relative_path))
				{
					const std::filesystem::path texture_path = g_AssetsPath / relative_path;
					const auto new_texture = TextureAssetManager::Instance().GetOrCreateTexture(
						PathToUtf8(texture_path));
					if (new_texture->IsLoaded())
						texture = new_texture;
					else
						EDITOR_LOG_WARN("Failed to load texture {0}.", PathToUtf8(texture_path.filename()));
				}
			}
			ImGui::EndDragDropTarget();
		}

		if (ImGui::IsItemHovered())
		{
			ImGui::BeginTooltip();
			ImGui::Image((ImTextureID)show_texture.get(), ImVec2(240, 240), ImVec2(0, 1), ImVec2(1, 0));
			ImGui::EndTooltip();
		}

		/* 文件名对齐到值列，弱化显示：它是"当前值"的说明而不是内容主体 */
		ImGui::SetCursorPosX(value_x);
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
		ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::TextDim);
		ImGui::TextUnformatted(RELATIVE_PATH(show_texture->GetPath()).c_str());
		ImGui::PopStyleColor();
		ImGui::PopTextWrapPos();

		EndPropertyRow();
	}

	/* 绘制一个可拖动的Int UI */
	void DrawDragIntUI(const char* label, int& value, float speed, float label_width)
	{
		PROFILE_FUNCTION();

		const float value_width = BeginPropertyRow(label, label_width);

		ImGui::SetNextItemWidth(value_width);
		ImGui::DragInt("##Int", &value, speed);

		EndPropertyRow();
	}

	/* 绘制一个可拖动的Float UI */
	void DrawDragFloatUI(const std::string& label, float& value, float speed, float label_width)
	{
		PROFILE_FUNCTION();

		const float value_width = BeginPropertyRow(label.c_str(), label_width);

		ImGui::SetNextItemWidth(value_width);
		ImGui::DragFloat("##Float", &value, speed);

		EndPropertyRow();
	}

	/* 绘制一个可拖动的Float2 UI */
	void DrawDragFloat2UI(const char* label, glm::vec2& value, float label_width)
	{
		PROFILE_FUNCTION();

		DrawVectorRow(label, glm::value_ptr(value), 2, 0.0f, label_width, "%.3f", false);
	}

	/* 绘制一个可拖动的Float3 UI */
	void DrawDragFloat3UI(const char* label, glm::vec3& value, float label_width)
	{
		PROFILE_FUNCTION();

		DrawVectorRow(label, glm::value_ptr(value), 3, 0.0f, label_width, "%.3f", false);
	}

	/* 绘制一个可拖动的Float4 UI */
	void DrawDragFloat4UI(const char* label, glm::vec4& value, float label_width)
	{
		PROFILE_FUNCTION();

		DrawVectorRow(label, glm::value_ptr(value), 4, 0.0f, label_width, "%.3f", false);
	}

	/* 绘制一个vec3 UI， 带XYZ */
	void DrawVec3ControlUI(const std::string& label, glm::vec3& values, float reset_value, float label_width,
	                       bool uniform_scale)
	{
		PROFILE_FUNCTION();

		/* 重置值由组件声明（如 Scale 复位成 1），点分量按钮即恢复；
		 * uniform_scale 行额外给 Shift 等比（见 DrawVectorRow） */
		DrawVectorRow(label.c_str(), glm::value_ptr(values), 3, reset_value, label_width, "%.2f", uniform_scale);
	}

	/* 绘制带选中的图像按钮UI */
	void DrawCheckedImageButtonUI(const std::string& label, const SharedPtr<DeviceTexture>& texture, const ImVec2& size, bool checked, const std::function<void()>& button_func)
	{
		PROFILE_FUNCTION();

		ASSERT(texture);

		/* 选中时样式 */
		if (checked)
		{
			const auto& colors = ImGui::GetStyle().Colors;
			const auto& button_active_color = colors[ImGuiCol_ButtonActive];
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(button_active_color.x, button_active_color.y, button_active_color.z, 0.5f));
		}

		/* Button */
		if (ImGui::ImageButton((ImTextureID)texture.get(), size, ImVec2(0, 1), ImVec2(1, 0), 0))
		{
			button_func();
		}

		/* 鼠标悬停提示 */
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(label.c_str());

		if (checked)
		{
			ImGui::PopStyleColor();
		}
	}

	/* 绘制一个Checkbox */
	bool DrawCheckboxUI(const std::string& label, bool& value, float label_width)
	{
		PROFILE_FUNCTION();

		BeginPropertyRow(label.c_str(), label_width);

		const bool changed = ImGui::Checkbox("##Checkbox", &value);

		EndPropertyRow();
		return changed;
	}

	/* 绘制一个Combo */
	void DrawComboUI(const std::string& label, const std::vector<std::string>& options, int& selected_idx, const std::function<void(int)>& callback, float label_width)
	{
		PROFILE_FUNCTION();

		ASSERT(!options.empty());

		const float value_width = BeginPropertyRow(label.c_str(), label_width);

		std::vector<const char*> option_strs;
		for (const auto& option : options)
			option_strs.emplace_back(option.c_str());

		const char* selected_option = option_strs[selected_idx];

		ImGui::SetNextItemWidth(value_width);
		if (ImGui::BeginCombo("##Combo", selected_option))
		{
			for (int i = 0; i < (int)options.size(); i++)
			{
				bool is_selectd = selected_option == option_strs[i];
				if (ImGui::Selectable(option_strs[i], is_selectd))
				{
					selected_option = option_strs[i];
					selected_idx = i;
					callback(i);
				}

				if (is_selectd)
					ImGui::SetItemDefaultFocus();
			}

			ImGui::EndCombo();
		}

		EndPropertyRow();
	}

	/* 绘制一个方向指示 */
	bool DrawDirectionIndicator(const std::string& label, glm::vec3& direction, float label_width)
	{
		PROFILE_FUNCTION();

		BeginPropertyRow(label.c_str(), label_width);

		ArrowWidget widget(glm::normalize(direction));
		const bool changed = widget.Draw();
		if (changed)
			direction = widget.GetDirection();

		EndPropertyRow();

		return changed;
	}

	/* 绘制一个询问弹窗UI(todo: 还不能用，需要调试) */
	bool DrawModalUI(const std::string& label, const std::string& content_text, bool& never_ask)
	{
		PROFILE_FUNCTION();

		/* 显示在窗口中间 */
		const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

		bool result = false;
		if (ImGui::BeginPopupModal(label.c_str(), NULL, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::TextWrapped(content_text.c_str());
			ImGui::Separator();

			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
			ImGui::Checkbox("Don't ask me next time", &never_ask);
			ImGui::PopStyleVar();

			if (ImGui::Button("OK", ImVec2(120, 0)))
			{
				ImGui::CloseCurrentPopup();
				result = true;
			}
			ImGui::SetItemDefaultFocus();
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(120, 0)))
			{
				ImGui::CloseCurrentPopup();
				result = false;
			}
			ImGui::EndPopup();
		}

		return result;
	}

	/* 圆点滑动条：细轨 + 圆点（当前值），交互全部沿用 ImGui 的 SliderBehavior（跟 SliderFloat
	 * 一字不差），区别只在绘制：没有控件框、没有矩形 grab、不在轨道上叠数值。两个量要对上：
	 * 圆点直径通过 GrabMinSize 压给 SliderBehavior（决定行程端点），细轨画在两端点之间 ——
	 * 拖到极值时圆点正好压住线头。 */
	bool DrawDotSliderFloat(const char* label, float& value, float v_min, float v_max,
		const char* format, ImGuiSliderFlags flags)
	{
		PROFILE_FUNCTION();

		ImGuiWindow* window = ImGui::GetCurrentWindow();
		if (window->SkipItems)
			return false;

		ImGuiContext& g = *ImGui::GetCurrentContext();
		const ImGuiStyle& style = g.Style;
		const ImGuiID id = window->GetID(label);
		const float width = ImGui::CalcItemWidth();

		const ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
		const ImRect frame_bb(window->DC.CursorPos,
			window->DC.CursorPos + ImVec2(width, label_size.y + style.FramePadding.y * 2.0f));
		const ImRect total_bb(frame_bb.Min, frame_bb.Max
			+ ImVec2(label_size.x > 0.0f ? style.ItemInnerSpacing.x + label_size.x : 0.0f, 0.0f));

		/* 命中区仍给足整个控件高（细线本身只有 3px，不然点不中） */
		const bool temp_input_allowed = (flags & ImGuiSliderFlags_NoInput) == 0;
		ImGui::ItemSize(total_bb, style.FramePadding.y);
		if (!ImGui::ItemAdd(total_bb, id, &frame_bb, temp_input_allowed ? ImGuiItemFlags_Inputable : 0))
			return false;

		if (format == nullptr)
			format = "%.0f";

		/* 激活与"转输入框"的判定照 SliderScalar：Ctrl+左键 / Tab 聚焦进入输入态 */
		const bool hovered = ImGui::ItemHoverable(frame_bb, id);
		bool temp_input_is_active = temp_input_allowed && ImGui::TempInputIsActive(id);
		if (!temp_input_is_active)
		{
			const bool input_requested_by_tabbing = temp_input_allowed
				&& (g.LastItemData.StatusFlags & ImGuiItemStatusFlags_FocusedByTabbing) != 0;
			const bool clicked = (hovered && g.IO.MouseClicked[0]);
			if (input_requested_by_tabbing || clicked || g.NavActivateId == id || g.NavActivateInputId == id)
			{
				ImGui::SetActiveID(id, window);
				ImGui::SetFocusID(id, window);
				ImGui::FocusWindow(window);
				g.ActiveIdUsingNavDirMask |= (1 << ImGuiDir_Left) | (1 << ImGuiDir_Right);
				if (input_requested_by_tabbing || (clicked && g.IO.KeyCtrl) || g.NavActivateInputId == id)
					temp_input_is_active = true;
			}
		}

		if (temp_input_is_active)
		{
			/* 与官方一致：只有 AlwaysClamp 才钳范围，否则输入可以越界 */
			const bool clamp_input = (flags & ImGuiSliderFlags_AlwaysClamp) != 0;
			return ImGui::TempInputScalar(frame_bb, id, label, ImGuiDataType_Float, &value, format,
				clamp_input ? &v_min : nullptr, clamp_input ? &v_max : nullptr);
		}

		/* 圆点直径 = 行程基准：压给 GrabMinSize，SliderBehavior 才知道圆点该停在哪儿 */
		const float dot_radius = EditorTheme::Token::SliderDotRadius;
		const float dot_diameter = dot_radius * 2.0f;

		ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, dot_diameter);

		ImRect grab_bb;
		const bool value_changed = ImGui::SliderBehavior(frame_bb, id, ImGuiDataType_Float,
			&value, &v_min, &v_max, format, flags, &grab_bb);

		ImGui::PopStyleVar();

		if (value_changed)
			ImGui::MarkItemEdited(id);

		/* 键盘焦点用细框：默认那档会向外扩出 4px，对一条细线来说太重 */
		ImGui::RenderNavHighlight(frame_bb, id, ImGuiNavHighlightFlags_TypeThin);

		const bool hot = hovered || g.ActiveId == id;
		const float track_y = (frame_bb.Min.y + frame_bb.Max.y) * 0.5f;
		const float track_h = EditorTheme::Token::SliderTrackThickness;

		/* 行程端点 = SliderBehaviorT 的 usable 区间（bb 两端各内收 grab_padding + 半个 grab） */
		constexpr float kGrabPadding = 2.0f; /* imgui_widgets.cpp: const float grab_padding = 2.0f; */
		const float grab_sz = ImMin(dot_diameter, frame_bb.GetWidth() - kGrabPadding * 2.0f);
		const float track_left = frame_bb.Min.x + kGrabPadding + grab_sz * 0.5f;
		const float track_right = frame_bb.Max.x - kGrabPadding - grab_sz * 0.5f;

		if (track_right > track_left)
		{
			const float dot_x = ImClamp((grab_bb.Min.x + grab_bb.Max.x) * 0.5f, track_left, track_right);
			const float y0 = track_y - track_h * 0.5f;
			const float y1 = track_y + track_h * 0.5f;
			const ImU32 track_col = ImGui::GetColorU32(EditorTheme::Token::SliderTrack);
			const ImU32 value_col = ImGui::GetColorU32(
				hot ? EditorTheme::Token::SliderHover : EditorTheme::Token::SliderValue);
			ImDrawList* draw_list = ImGui::GetWindowDrawList();

			/* 顺序即遮挡：细轨 → 点亮段 → 圆点 */
			draw_list->AddRectFilled(ImVec2(track_left, y0), ImVec2(track_right, y1),
				track_col, track_h * 0.5f);

			/* 点亮段：行程左端 → 圆点中心；右端藏进圆点里，不用收口 */
			if (dot_x > track_left + 0.5f)
				draw_list->AddRectFilled(ImVec2(track_left, y0), ImVec2(dot_x, y1),
					value_col, track_h * 0.5f);

			draw_list->AddCircleFilled(ImVec2(dot_x, track_y), dot_radius, value_col);
		}

		if (label_size.x > 0.0f)
			ImGui::RenderText(ImVec2(frame_bb.Max.x + style.ItemInnerSpacing.x,
				frame_bb.Min.y + style.FramePadding.y), label);

		return value_changed;
	}

}
