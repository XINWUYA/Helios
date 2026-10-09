#pragma once
/* 编辑器面板的通用版式（header-only）：顶部行和分组卡片都在这定义一次，各面板只管描述内容。
 * 面板不画静态标题（名字由页签给出）。
 * 卡身里别用 ImGui::Columns / ChannelsSplit：卡片背景要等画完内容再补，通道不能套通道。 */

#include <string>
#include <imgui.h>
/* 卡片背景要用 ImGuiWindow::InnerRect（滚动条之外的可用区域），故需要内部头 */
#include <imgui_internal.h>
#include "EditorIcons.h"
#include "Helios/ImGui/EditorTheme.h"

namespace Helios::PanelChrome
{
	/* ==================== 顶部一行 ==================== */

	struct HeaderRow
	{
		ImVec2 Min{ 0.0f, 0.0f };   /* 行左上角（屏幕坐标） */
		float  Right{ 0.0f };       /* 内容区右端 x（右端动作按它定位） */
		float  Height{ 0.0f };      /* 行高 */
		float  TitleX{ 0.0f };      /* 标题起点 x（图标之后） */
	};

	/* 开一行头部：画图标（None 不画），光标停在标题起点，标题由调用方自己画（静态标题用 DrawHeaderTitle）。
	 * leading > 0 时在图标前留出一格（行首的前置控件），图标和标题起点一起右移。 */
	inline HeaderRow BeginHeaderRow(Icons::Id icon, float leading = 0.0f)
	{
		const ImGuiStyle& style = ImGui::GetStyle();

		HeaderRow header;
		header.Min = ImGui::GetCursorScreenPos();
		header.Right = header.Min.x + ImGui::GetContentRegionAvail().x;
		header.Height = ImGui::GetFrameHeight();

		const float icon_x = header.Min.x + leading;

		if (icon == Icons::Id::None)
		{
			header.TitleX = icon_x;
			ImGui::SetCursorScreenPos(ImVec2(header.TitleX, header.Min.y));
			return header;
		}

		/* 图标用标题字号：跟标题同一行时视觉重量才对得上 */
		ImGui::PushFont(EditorTheme::GetFonts().Title);
		const float icon_size = ImGui::GetFontSize();
		ImGui::PopFont();

		Icons::Draw(ImGui::GetWindowDrawList(), icon,
			ImVec2(icon_x + icon_size * 0.5f, header.Min.y + header.Height * 0.5f),
			icon_size, ImGui::GetColorU32(EditorTheme::Token::TextLabel));

		header.TitleX = icon_x + icon_size + style.ItemInnerSpacing.x;
		ImGui::SetCursorScreenPos(ImVec2(header.TitleX, header.Min.y));
		return header;
	}

	/* 头部标题：标题字体 + 正文色，是面板里最重的一行文字。
	 * width > 0 时按它裁剪 —— 右端还有动作按钮时用它防止文字压上去。 */
	inline void DrawHeaderTitle(const HeaderRow& header, const std::string& title, float width = 0.0f)
	{
		ImGui::PushFont(EditorTheme::GetFonts().Title);

		const float title_size = ImGui::GetFontSize();
		const float title_y = header.Min.y + (header.Height - title_size) * 0.5f;

		if (width > 0.0f)
			ImGui::PushClipRect(ImVec2(header.TitleX, title_y),
				ImVec2(header.TitleX + width, title_y + title_size), true);

		ImGui::SetCursorScreenPos(ImVec2(header.TitleX, title_y));
		ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Token::Text);
		ImGui::TextUnformatted(title.c_str());
		ImGui::PopStyleColor();

		if (width > 0.0f)
			ImGui::PopClipRect();

		ImGui::PopFont();
	}

	/* 右端动作位的宽度（含一格内边距）：算标题可用宽度时减掉它 */
	inline float HeaderActionWidth(float button_size, int action_count = 1)
	{
		return (button_size + ImGui::GetStyle().ItemInnerSpacing.x) * static_cast<float>(action_count);
	}

	/* 把光标移到头部行右端第 slot_from_right 个动作位（0 是最靠右的那个） */
	inline void PlaceHeaderAction(const HeaderRow& header, float size, int slot_from_right = 0)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const float x = header.Right - size - style.ItemInnerSpacing.x * static_cast<float>(slot_from_right)
			- size * static_cast<float>(slot_from_right);
		ImGui::SetCursorScreenPos(ImVec2(x, header.Min.y));
	}

	/* 收尾：一条分隔线把头部与内容分开，光标随之落到下一行。
	 * 连续两行头部时（头部 + 工具行），第二行传 separator = false。 */
	inline void EndHeaderRow(const HeaderRow& header, bool separator = true)
	{
		ImGui::SetCursorScreenPos(ImVec2(header.Min.x, header.Min.y + header.Height));

		if (separator)
			ImGui::Separator();
	}

	/* ==================== 树行内容 ==================== */

	/* 只读单行文本，宽度由调用方限定（width）：超宽就用省略号收尾，排版宽度被占位框夹在 width 内。
	 * 别用 PushClipRect + TextUnformatted：它只裁绘制不裁布局，超宽文本会把面板撑出横向可滑区。 */
	inline void DrawClippedTextLine(const char* text, float width)
	{
		const ImVec2 cursor = ImGui::GetCursorScreenPos();
		const ImVec2 text_pos(cursor.x, cursor.y + ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset);
		const ImVec2 text_size = ImGui::CalcTextSize(text);

		if (text_size.x <= width)
		{
			ImGui::TextUnformatted(text);
			return;
		}

		/* 放不下：省略号收尾。占位与文本行同高、只占裁剪后的那一截宽，
		 * 行内容与行右端的尾随控件不会重叠。 */
		const float avail = ImMax(width, 1.0f);
		ImGui::Dummy(ImVec2(avail, ImGui::GetTextLineHeight()));
		ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), text_pos,
			ImVec2(text_pos.x + avail, text_pos.y + ImGui::GetTextLineHeight()),
			text_pos.x + avail, text_pos.x + avail, text, nullptr, &text_size);
	}

	/* 树节点行内容：图标 + 文本（marked 时带脏标记）。要在 TreeNodeEx 之后紧接着调用，
	 * TreeNode 的标签传空字符串 ""（不能传 "##"）；点击 / 拖拽 / 右键也都得在它后面立刻处理。
	 * clip_right > 0 时把文本裁到该 x 之前，给行右端留出尾随控件的位置（走 DrawClippedTextLine）。 */
	inline void DrawTreeRowLabel(Icons::Id icon, const std::string& text, bool marked = false,
	                             ImVec4 icon_color = EditorTheme::Token::Text, float clip_right = 0.0f)
	{
		const ImGuiStyle& style = ImGui::GetStyle();

		/* 空标签的 TreeNode 末尾恰好落在「行首 + 一个字号」处（箭头右侧），
		 * 从这里再让出一点内边距，图标才不贴着箭头 */
		ImGui::SameLine(0.0f, style.ItemInnerSpacing.x + style.FramePadding.x);

		const float icon_size = ImGui::GetFontSize();
		ImGui::Dummy(ImVec2(icon_size, icon_size));

		const ImVec2 icon_min = ImGui::GetItemRectMin();
		const ImVec2 icon_max = ImGui::GetItemRectMax();
		Icons::Draw(ImGui::GetWindowDrawList(), icon,
			ImVec2((icon_min.x + icon_max.x) * 0.5f, (icon_min.y + icon_max.y) * 0.5f),
			icon_size, ImGui::GetColorU32(icon_color));

		ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);

		/* 有尾随控件（clip_right > 0）时按"到 clip_right 为止"的宽度裁；没有就不限宽 */
		if (clip_right > 0.0f)
			DrawClippedTextLine(text.c_str(), clip_right - ImGui::GetCursorScreenPos().x);
		else
			ImGui::TextUnformatted(text.c_str());

		if (!marked)
			return;

		/* 脏标记：名字后点一个实心圆点（自绘，不依赖字体里有没有 '•'） */
		ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);

		const float dot_radius = ImMax(1.5f, ImGui::GetFontSize() * 0.16f);
		ImGui::Dummy(ImVec2(dot_radius * 2.0f, ImGui::GetFontSize()));

		const ImVec2 dot_min = ImGui::GetItemRectMin();
		const ImVec2 dot_max = ImGui::GetItemRectMax();
		ImGui::GetWindowDrawList()->AddCircleFilled(
			ImVec2((dot_min.x + dot_max.x) * 0.5f, (dot_min.y + dot_max.y) * 0.5f),
			dot_radius, ImGui::GetColorU32(EditorTheme::Token::Text));
	}

	/* ==================== 菜单行内容 ==================== */

	/* 带图标的菜单行（下拉条目）：整行命中 / 悬停高亮 / 点击收起都跟 MenuItem 一样，
	 * 只是文字前多一枚矢量图标；行宽要自己算（弹层是 AlwaysAutoResize，控件多宽弹层就多宽）。 */
	inline bool MenuItemWithIcon(Icons::Id icon, const char* label)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const float icon_size = ImGui::GetFontSize();
		const float icon_gap = style.ItemInnerSpacing.x;

		const ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
		const float width = style.FramePadding.x * 2.0f + icon_size + icon_gap + label_size.x;

		ImGui::PushID(label);
		const bool clicked = ImGui::Selectable("##row", false,
			ImGuiSelectableFlags_SelectOnRelease | ImGuiSelectableFlags_SetNavIdOnHover
				| ImGuiSelectableFlags_SpanAvailWidth,
			ImVec2(width, 0.0f));
		ImGui::PopID();

		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		const float center_y = (min.y + max.y) * 0.5f;

		ImDrawList* const draw_list = ImGui::GetWindowDrawList();

		/* 图标与文字都画在 Selectable 之后：悬停底色已经落好，不会被盖住 */
		Icons::Draw(draw_list, icon,
			ImVec2(min.x + style.FramePadding.x + icon_size * 0.5f, center_y),
			icon_size, ImGui::GetColorU32(EditorTheme::Token::TextLabel));

		draw_list->AddText(
			ImVec2(min.x + style.FramePadding.x + icon_size + icon_gap,
				center_y - ImGui::GetFontSize() * 0.5f),
			ImGui::GetColorU32(EditorTheme::Token::Text), label);

		return clicked;
	}

	/* 行尾勾选标记：行右缘固定列里的一枚矢量对勾（两笔画，Accent 色）——
	 * 切换行（MenuItemToggleWithIcon）与单选行（MenuItemSelectWithIcon）共用，
	 * 两处的"勾选列"逐像素一致；未勾选的行也保留同一列，位置不随状态跳。 */
	inline void DrawMenuRowCheck(ImDrawList* draw_list, const ImVec2& row_max, float center_y, float icon_size)
	{
		const float check_center_x = row_max.x - ImGui::GetStyle().FramePadding.x - icon_size * 0.5f;
		const float thickness = ImMax(1.5f, icon_size * 0.105f);
		const ImU32 check_color = ImGui::GetColorU32(EditorTheme::Token::Accent);
		const ImVec2 stroke_a(check_center_x - icon_size * 0.30f, center_y + icon_size * 0.03f);
		const ImVec2 stroke_b(check_center_x - icon_size * 0.08f, center_y + icon_size * 0.24f);
		const ImVec2 stroke_c(check_center_x + icon_size * 0.27f, center_y - icon_size * 0.26f);
		draw_list->AddLine(stroke_a, stroke_b, check_color, thickness);
		draw_list->AddLine(stroke_b, stroke_c, check_color, thickness);
	}

	/* 带图标和勾选的切换菜单行（Gizmos 显隐那一类）：整行命中 / 悬停高亮跟 MenuItemWithIcon 一样；
	 * 行右固定画一枚 Accent 色对勾，点击翻转 *value、不收起弹层。行高取控件档，行宽铺满弹层
	 * （SpanAvailWidth），各行的勾选列对齐在一条竖线上。 */
	inline bool MenuItemToggleWithIcon(Icons::Id icon, const char* label, bool* value)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const float icon_size = ImGui::GetFontSize();
		const float icon_gap = style.ItemInnerSpacing.x;

		/* 与 MenuItemWithIcon 同一笔账，尾部再加一个图标宽的勾选格 */
		const ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
		const float width = style.FramePadding.x * 2.0f + icon_size + icon_gap + label_size.x
			+ icon_gap + icon_size;

		ImGui::PushID(label);
		const bool clicked = ImGui::Selectable("##row", false,
			ImGuiSelectableFlags_SelectOnRelease | ImGuiSelectableFlags_SetNavIdOnHover
				| ImGuiSelectableFlags_SpanAvailWidth | ImGuiSelectableFlags_DontClosePopups,
			ImVec2(width, ImGui::GetFrameHeight()));
		ImGui::PopID();

		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		const float center_y = (min.y + max.y) * 0.5f;

		ImDrawList* const draw_list = ImGui::GetWindowDrawList();

		/* 图标与文字都画在 Selectable 之后：悬停底色已经落好，不会被盖住 */
		Icons::Draw(draw_list, icon,
			ImVec2(min.x + style.FramePadding.x + icon_size * 0.5f, center_y),
			icon_size, ImGui::GetColorU32(EditorTheme::Token::TextLabel));

		draw_list->AddText(
			ImVec2(min.x + style.FramePadding.x + icon_size + icon_gap,
				center_y - ImGui::GetFontSize() * 0.5f),
			ImGui::GetColorU32(EditorTheme::Token::Text), label);

		/* 勾选标记：矢量对勾（两笔画）——勾选的画在行右缘的固定列里，
		 * 未勾选也保留同一列（位置不随状态跳） */
		if (value != nullptr && *value)
			DrawMenuRowCheck(draw_list, max, center_y, icon_size);

		if (clicked && value != nullptr)
			*value = !*value;

		return clicked;
	}

	/* 三态复选框菜单行（"全选"这种聚合开关）：行右缘画三态复选框（全开 = 强调色实底 + 勾、
	 * 部分开 = 实底 + 横杠、全关 = 空框）。版式跟其它菜单行一样（控件档行高、整行命中、点击不收起）；
	 * 这一行自己不翻转任何值，只返回 clicked、由调用方决定。 */
	inline bool MenuItemTristateBox(bool all_on, bool none_on)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const float icon_size = ImGui::GetFontSize();
		const float box_half = ImMax(6.0f, icon_size * 0.54f);

		/* 行宽＝三态框的内容宽；弹层宽度由分项行与最小宽约束撑足 */
		const float width = style.FramePadding.x * 2.0f + box_half * 2.0f;

		ImGui::PushID("tristate");
		const bool clicked = ImGui::Selectable("##row", false,
			ImGuiSelectableFlags_SelectOnRelease | ImGuiSelectableFlags_SetNavIdOnHover
				| ImGuiSelectableFlags_SpanAvailWidth | ImGuiSelectableFlags_DontClosePopups,
			ImVec2(width, ImGui::GetFrameHeight()));
		ImGui::PopID();

		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		const float center_y = (min.y + max.y) * 0.5f;

		ImDrawList* const draw_list = ImGui::GetWindowDrawList();

		/* 三态复选框：外框在行右缘的勾选列（与下方各行的对勾同一列）。
		 * 勾选态 = 强调色实底圆角方片 + 深色记号（呼应 Theme "激活 = 强调色"）；
		 * 未勾选态 = 中性底 + 细描边空框（与原生 Checkbox 的框体同色系） */
		const float box_center_x = max.x - style.FramePadding.x - icon_size * 0.5f;
		const ImVec2 box_min(box_center_x - box_half, center_y - box_half);
		const ImVec2 box_max(box_center_x + box_half, center_y + box_half);
		constexpr float kBoxRounding = 4.0f;

		if (all_on || !none_on)
		{
			draw_list->AddRectFilled(box_min, box_max,
				ImGui::GetColorU32(EditorTheme::Token::Accent), kBoxRounding);

			const ImU32 mark_color = ImGui::GetColorU32(EditorTheme::Token::Neutral0);
			if (all_on)
			{
				/* 勾：ImGui::RenderCheckMark 的几何（内缩、三段折线、sz/5 笔画） */
				const float pad = box_half * 0.30f;
				float sz = box_half * 2.0f - pad * 2.0f;
				const float mark_thickness = ImMax(sz / 5.0f, 1.0f);
				sz -= mark_thickness * 0.5f;
				const float third = sz / 3.0f;
				const float bx = box_min.x + pad + mark_thickness * 0.25f + third;
				const float by = box_min.y + pad + mark_thickness * 0.25f + sz - third * 0.5f;
				draw_list->PathLineTo(ImVec2(bx - third, by - third));
				draw_list->PathLineTo(ImVec2(bx, by));
				draw_list->PathLineTo(ImVec2(bx + third * 2.0f, by - third * 2.0f));
				draw_list->PathStroke(mark_color, 0, mark_thickness);
			}
			else
			{
				/* 部分开：居中横杠（走 PathStroke —— ImDrawList::AddLine 会给端点
				 * 加 (0.5, 0.5) 像素偏移，粗描边下破坏居中） */
				const float mark_thickness = ImMax(1.5f, box_half * 0.30f);
				draw_list->PathLineTo(ImVec2(box_center_x - box_half * 0.44f, center_y));
				draw_list->PathLineTo(ImVec2(box_center_x + box_half * 0.44f, center_y));
				draw_list->PathStroke(mark_color, 0, mark_thickness);
			}
		}
		else
		{
			draw_list->AddRectFilled(box_min, box_max,
				ImGui::GetColorU32(EditorTheme::Token::Neutral4), kBoxRounding);
			draw_list->AddRect(box_min, box_max,
				ImGui::GetColorU32(EditorTheme::Token::Neutral6), kBoxRounding, 0, 1.25f);
		}

		return clicked;
	}

	/* 带图标的单选菜单行（"一组里选一个"的弹层条目）：外观跟 MenuItemToggleWithIcon 一样；
	 * 选中态由调用方传进来（本行不翻转值），点击不收起弹层。enabled = false 时整行禁用，
	 * 图标 / 文字 / 对勾跟着 BeginDisabled 一起变淡。 */
	inline bool MenuItemSelectWithIcon(Icons::Id icon, const char* label, bool selected, bool enabled = true)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const float icon_size = ImGui::GetFontSize();
		const float icon_gap = style.ItemInnerSpacing.x;

		/* 与 MenuItemToggleWithIcon 同一笔账，尾部再加一个图标宽的勾选格 */
		const ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
		const float width = style.FramePadding.x * 2.0f + icon_size + icon_gap + label_size.x
			+ icon_gap + icon_size;

		if (!enabled)
			ImGui::BeginDisabled();

		ImGui::PushID(label);
		const bool clicked = ImGui::Selectable("##row", false,
			ImGuiSelectableFlags_SelectOnRelease | ImGuiSelectableFlags_SetNavIdOnHover
				| ImGuiSelectableFlags_DontClosePopups | ImGuiSelectableFlags_SpanAvailWidth,
			ImVec2(width, ImGui::GetFrameHeight()));
		ImGui::PopID();

		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		const float center_y = (min.y + max.y) * 0.5f;

		ImDrawList* const draw_list = ImGui::GetWindowDrawList();

		/* 图标与文字都画在 Selectable 之后：悬停底色已经落好，不会被盖住 */
		Icons::Draw(draw_list, icon,
			ImVec2(min.x + style.FramePadding.x + icon_size * 0.5f, center_y),
			icon_size, ImGui::GetColorU32(EditorTheme::Token::TextLabel));

		draw_list->AddText(
			ImVec2(min.x + style.FramePadding.x + icon_size + icon_gap,
				center_y - ImGui::GetFontSize() * 0.5f),
			ImGui::GetColorU32(EditorTheme::Token::Text), label);

		if (selected)
			DrawMenuRowCheck(draw_list, max, center_y, icon_size);

		if (!enabled)
			ImGui::EndDisabled();

		return clicked;
	}

	/* 菜单分区头（弹层里的分组标题）：不可点的小标题 —— 小一号大写字母 + 右侧一条细线。
	 * 行宽取内容区右缘（auto-resize 弹层从第二帧起就稳定了）。 */
	inline void MenuSectionHeader(const char* label)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const ImVec2 min = ImGui::GetCursorScreenPos();
		const float width = ImGui::GetContentRegionAvail().x;
		const float height = ImGui::GetTextLineHeight() + style.FramePadding.y + style.ItemSpacing.y;
		ImGui::Dummy(ImVec2(width, height));

		ImDrawList* const draw_list = ImGui::GetWindowDrawList();
		ImFont* const font = ImGui::GetFont();
		const float font_size = ImGui::GetFontSize() * 0.85f;
		const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, label);

		draw_list->AddText(font, font_size,
			ImVec2(min.x, min.y + (height - text_size.y) * 0.5f),
			ImGui::GetColorU32(EditorTheme::Token::TextDim), label);

		const float line_y = min.y + height * 0.5f;
		const float line_x = min.x + text_size.x + style.ItemInnerSpacing.x * 2.0f;
		draw_list->AddLine(ImVec2(line_x, line_y), ImVec2(min.x + width, line_y),
			ImGui::GetColorU32(EditorTheme::Token::Separator), 1.0f);
	}

	/* 带图标的子菜单行（「新建」下拉里的分组行）：外观 = 菜单行 + 右缘一个右向箭头；
	 * 行为照搬 ImGui BeginMenu 的垂直弹层路径（悬停展开、链式关闭、ChildMenu）。
	 * 只给弹层菜单用；键盘导航略过（这些菜单的键盘入口是搜索框）。 */
	inline bool BeginMenuWithIcon(Icons::Id icon, const char* label)
	{
		ImGuiContext& g = *ImGui::GetCurrentContext();
		ImGuiWindow* const window = g.CurrentWindow;
		if (window->SkipItems)
			return false;

		const ImGuiStyle& style = g.Style;
		const ImGuiID id = window->GetID(label);
		bool menu_is_open = ImGui::IsPopupOpen(id, ImGuiPopupFlags_None);

		/* 子菜单弹层旗标与原生一致：ChildMenu 让鼠标能"跨菜单悬停"
		 *（不设的话上层弹层会把悬停抢走，指针进不了子菜单）。 */
		ImGuiWindowFlags flags = ImGuiWindowFlags_ChildMenu | ImGuiWindowFlags_AlwaysAutoResize
			| ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings
			| ImGuiWindowFlags_NoNavFocus;
		if (window->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_ChildMenu))
			flags |= ImGuiWindowFlags_ChildWindow;

		/* ---- 行：整行命中（Selectable），图标 / 文本 / 箭头画在它之后 ----
		 * Selectable 用空 label：空串的 ID 就是 ID 栈顶 —— 恰是 GetID(label) 的值
		 *（PushID(label) 把它压上来），于是行 id 与 popup id 同源、悬停判定对得上。 */
		const float icon_size = ImGui::GetFontSize();
		const float icon_gap = style.ItemInnerSpacing.x;
		const ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
		const float width = style.FramePadding.x * 2.0f + icon_size + icon_gap + label_size.x
			+ icon_gap + icon_size;   /* 尾部留一个图标宽的箭头格 */

		const ImVec2 pos = window->DC.CursorPos;
		const ImVec2 popup_pos(pos.x, pos.y - style.WindowPadding.y);

		ImGui::PushID(label);
		const bool pressed = ImGui::Selectable("", menu_is_open,
			ImGuiSelectableFlags_NoHoldingActiveID | ImGuiSelectableFlags_SelectOnClick
				| ImGuiSelectableFlags_DontClosePopups | ImGuiSelectableFlags_SpanAvailWidth,
			ImVec2(width, 0.0f));
		ImGui::PopID();

		const bool hovered = (g.HoveredId == id);

		{
			const ImVec2 min = ImGui::GetItemRectMin();
			const ImVec2 max = ImGui::GetItemRectMax();
			const float center_y = (min.y + max.y) * 0.5f;
			ImDrawList* const draw_list = ImGui::GetWindowDrawList();

			/* 图标与文本画在 Selectable 之后（悬停底色已经落好）——
			 * 起点公式与 MenuItemWithIcon 一致，图标列 / 文本列自然对齐。 */
			Icons::Draw(draw_list, icon,
				ImVec2(min.x + style.FramePadding.x + icon_size * 0.5f, center_y),
				icon_size, ImGui::GetColorU32(EditorTheme::Token::TextLabel));

			draw_list->AddText(
				ImVec2(min.x + style.FramePadding.x + icon_size + icon_gap,
					center_y - ImGui::GetFontSize() * 0.5f),
				ImGui::GetColorU32(EditorTheme::Token::Text), label);

			/* 右向箭头：矢量笔画（两段线），画在行右缘的固定格（与勾选列同一位置语言） */
			const float arrow_x = max.x - style.FramePadding.x - icon_size * 0.5f;
			const float arrow_d = icon_size * 0.22f;
			const float arrow_t = ImMax(1.5f, icon_size * 0.105f);
			const ImU32 arrow_color = ImGui::GetColorU32(EditorTheme::Token::TextLabel);
			draw_list->AddLine(ImVec2(arrow_x - arrow_d, center_y - arrow_d),
				ImVec2(arrow_x, center_y), arrow_color, arrow_t);
			draw_list->AddLine(ImVec2(arrow_x, center_y),
				ImVec2(arrow_x - arrow_d, center_y + arrow_d), arrow_color, arrow_t);
		}

		/* ---- 开合（照抄 BeginMenuEx 的 vertical 分支） ---- */
		bool want_open = false;
		bool want_close = false;

		/* "斜向驶向子菜单"的三角通道：指针离开本行、沿对角去子菜单的途中
		 * 不该把子菜单关掉（否则穿过邻行的一瞬就闪没了） */
		bool moving_toward_child_menu = false;
		ImGuiWindow* child_menu_window = (g.BeginPopupStack.Size < g.OpenPopupStack.Size
			&& g.OpenPopupStack[g.BeginPopupStack.Size].SourceWindow == window)
			? g.OpenPopupStack[g.BeginPopupStack.Size].Window : nullptr;
		if (g.HoveredWindow == window && child_menu_window != nullptr
			&& !(window->Flags & ImGuiWindowFlags_MenuBar))
		{
			const float ref_unit = g.FontSize;
			const ImRect next_window_rect = child_menu_window->Rect();
			/* 不用 ImVec2 运算符（operator- 要求 IMGUI_DEFINE_MATH_OPERATORS 在
			 * 包含 imgui.h 时已生效，本头文件的使用方不保证这个前提），显式分量相减 */
			ImVec2 ta(g.IO.MousePos.x - g.IO.MouseDelta.x, g.IO.MousePos.y - g.IO.MouseDelta.y);
			ImVec2 tb = (window->Pos.x < child_menu_window->Pos.x) ? next_window_rect.GetTL() : next_window_rect.GetTR();
			ImVec2 tc = (window->Pos.x < child_menu_window->Pos.x) ? next_window_rect.GetBL() : next_window_rect.GetBR();
			const float extra = ImClamp(ImFabs(ta.x - tb.x) * 0.30f, ref_unit * 0.5f, ref_unit * 2.5f);
			ta.x += (window->Pos.x < child_menu_window->Pos.x) ? -0.5f : +0.5f;
			tb.y = ta.y + ImMax((tb.y - extra) - ta.y, -ref_unit * 8.0f);
			tc.y = ta.y + ImMin((tc.y + extra) - ta.y, +ref_unit * 8.0f);
			moving_toward_child_menu = ImTriangleContainsPoint(ta, tb, tc, g.IO.MousePos);
		}
		if (menu_is_open && !hovered && g.HoveredWindow == window
			&& g.HoveredIdPreviousFrame != 0 && g.HoveredIdPreviousFrame != id
			&& !moving_toward_child_menu)
			want_close = true;

		if (!menu_is_open && (pressed || (hovered && !moving_toward_child_menu)))
			want_open = true;

		if (want_close && ImGui::IsPopupOpen(id, ImGuiPopupFlags_None))
			ImGui::ClosePopupToLevel(g.BeginPopupStack.Size, true);

		if (!menu_is_open && want_open && g.OpenPopupStack.Size > g.BeginPopupStack.Size)
		{
			/* 别的同级子菜单还开着：先请它让位，本帧不再展开（照抄原生 ——
			 * 别在同一个帧里回收同级的菜单层，让出去一帧） */
			ImGui::OpenPopupEx(id, ImGuiPopupFlags_None);
			return false;
		}

		menu_is_open |= want_open;
		if (want_open)
			ImGui::OpenPopupEx(id, ImGuiPopupFlags_None);

		if (menu_is_open)
		{
			/* popup_pos 只是 FindBestWindowPosForPopup 的参考点（最终位置由弹层自选） */
			ImGui::SetNextWindowPos(popup_pos, ImGuiCond_Always);
			menu_is_open = ImGui::BeginPopupEx(id, flags);
		}
		else
		{
			g.NextWindowData.ClearFlags();
		}

		return menu_is_open;
	}

	/* 配对收尾（= EndMenu 的 EndPopup 部分；键盘 nav 的 Left 关闭逻辑不参与，从略） */
	inline void EndMenuWithIcon()
	{
		ImGui::EndPopup();
	}

	/* ==================== 可拖拽分隔条 ==================== */

	/* 左右两块之间的拖拽分隔条：按住左右拖，按 delta.x 改「左块占比」。
	 * ratio 由调用方持久化，钳在 [min_ratio, max_ratio] 之间；height 是命中区高度（给足整栏就行）。 */
	inline bool HorizontalSplitter(float& ratio, float total_width, float min_ratio, float max_ratio,
	                               float width, float height)
	{
		ImDrawList* draw_list = ImGui::GetWindowDrawList();

		const ImVec2 min = ImGui::GetCursorScreenPos();
		const float bar_width = ImMax(width, 1.0f);
		const float bar_height = ImMax(height, 1.0f);

		ImGui::InvisibleButton("##PanelSplitter", ImVec2(bar_width, bar_height));

		const bool hovered = ImGui::IsItemHovered();
		const bool active = ImGui::IsItemActive();

		if (hovered || active)
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

		if (active && total_width > 0.0f)
			ratio = ImClamp(ratio + ImGui::GetIO().MouseDelta.x / total_width, min_ratio, max_ratio);

		/* 常态是一条与背景边框同色的细线，只有指向它时才显形（悬停加亮、拖动用强调色） */
		const ImVec4 color = active ? EditorTheme::Token::Accent
			: (hovered ? EditorTheme::Token::TextDim : EditorTheme::Token::Border);
		const float line_x = min.x + bar_width * 0.5f;
		draw_list->AddLine(ImVec2(line_x, min.y), ImVec2(line_x, min.y + bar_height),
			ImGui::GetColorU32(color));

		return active;
	}

	/* ==================== 分组卡片 ==================== */

	/* 卡片背景往外扩的留白；容器内边距取这个值，卡片背景就贴住容器两边。
	 * 要用主题常量、别现算（按窗口内边距 × 0.6 现算的话，容器设过内边距后就不对了）。 */
	inline float CardPad()
	{
		return EditorTheme::Token::CardPad;
	}

	struct Card
	{
		ImVec2 Min{ 0.0f, 0.0f };   /* 卡头行左上角（= 面板内容区原点） */
		float  Width{ 0.0f };       /* 内容区宽度 */
		float  Right{ 0.0f };       /* 卡右端 x */
		float  HeaderHeight{ 0.0f };
		float  ContentX{ 0.0f };    /* 卡头标题之后的 x */
		float  Pad{ 0.0f };         /* 卡内留白（背景向左右各外扩这么多） */
		bool   Open{ true };
		bool   ActionClicked{ false };  /* 卡头右端的动作按钮被点了（调用方据此开菜单） */
		bool   HeaderHovered{ false };  /* 卡头整行（含动作位）是否悬停 —— 悬停要提亮 */
	};

	/* 折叠指示三角：朝下 = 展开、朝右 = 折叠；两个方向是同一个三角形转 90°
	 * （沿用 ImGui 箭头 0.866 / 0.75 的比例），免得切换方向时看起来变小。 */
	inline void DrawDisclosureArrow(ImDrawList* draw_list, const ImVec2& center, float size, bool open, ImU32 color)
	{
		const float r = size * 0.5f; /* size 是箭头占据的方框边长 */

		const ImVec2 a = open ? ImVec2(0.000f, 0.750f) : ImVec2(0.750f, 0.000f);
		const ImVec2 b = open ? ImVec2(-0.866f, -0.750f) : ImVec2(-0.750f, 0.866f);
		const ImVec2 c = open ? ImVec2(0.866f, -0.750f) : ImVec2(-0.750f, -0.866f);

		draw_list->AddTriangleFilled(
			ImVec2(center.x + a.x * r, center.y + a.y * r),
			ImVec2(center.x + b.x * r, center.y + b.y * r),
			ImVec2(center.x + c.x * r, center.y + c.y * r), color);
	}

	/* 开一张卡片：卡头（折叠箭头 + 可选图标 + 标题 + 右端动作）+ 卡身；动作按钮由卡片自己提交。
	 * 折叠状态存在窗口级存储里、键 = 标题（连同调用方 ID 作用域）；同名卡要调用方自己 PushID 区分。 */
	inline Card BeginCard(const char* title, Icons::Id title_icon = Icons::Id::None,
	                      Icons::Id action_icon = Icons::Id::None, const char* action_tooltip = nullptr,
	                      bool default_open = true)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		ImDrawList* draw_list = ImGui::GetWindowDrawList();

		Card card;
		card.Min = ImGui::GetCursorScreenPos();
		card.Width = ImGui::GetContentRegionAvail().x;
		card.Right = card.Min.x + card.Width;
		card.HeaderHeight = ImGui::GetFrameHeight();
		card.Pad = CardPad();

		/* 卡的身份 = 标题：折叠键 / 卡头命中区 / 动作按钮都挂在这一层作用域；
		 * 这一层只包卡头，卡身内容的 ID 不受影响。 */
		ImGui::PushID(title);
		bool* open = ImGui::GetStateStorage()->GetBoolRef(ImGui::GetID("##CardOpen"), default_open);

		draw_list->ChannelsSplit(2);
		draw_list->ChannelsSetCurrent(1); /* 内容画在上层，卡背景稍后补在下层 */

		/* ---- 卡头：命中区与右端动作位各占一段，互不重叠 ---- */
		const float action_width = (action_icon != Icons::Id::None) ? card.HeaderHeight : 0.0f;
		ImGui::InvisibleButton("##CardHeader",
			ImVec2(ImMax(card.Width - action_width, 1.0f), card.HeaderHeight));

		const bool header_hovered = ImGui::IsItemHovered();
		if (ImGui::IsItemClicked())
			*open = !*open;

		bool action_hovered = false;
		if (action_width > 0.0f)
		{
			ImGui::SameLine(0.0f, 0.0f);
			card.ActionClicked = Icons::IconButton(action_icon,
				ImVec2(action_width, card.HeaderHeight), false, action_tooltip);
			action_hovered = ImGui::IsItemHovered();
		}
		ImGui::PopID();

		/* 箭头 + 图标 + 标题自绘：不参与 ImGui 布局，整行命中区才不会被装饰元素切开 */
		const float text_height = ImGui::GetFontSize();
		const float center_y = card.Min.y + card.HeaderHeight * 0.5f;
		const float arrow_size = text_height * 0.55f; /* 箭头占一个方框，与图标同高但更小 */
		float cursor_x = card.Min.x;

		DrawDisclosureArrow(draw_list, ImVec2(cursor_x + arrow_size * 0.5f, center_y),
			arrow_size, *open, ImGui::GetColorU32(EditorTheme::Token::TextDim));
		cursor_x += arrow_size + style.ItemInnerSpacing.x;

		if (title_icon != Icons::Id::None)
		{
			Icons::Draw(draw_list, title_icon, ImVec2(cursor_x + text_height * 0.5f, center_y),
				text_height, ImGui::GetColorU32(EditorTheme::Token::TextLabel));
			cursor_x += text_height + style.ItemInnerSpacing.x;
		}

		draw_list->AddText(ImVec2(cursor_x, center_y - text_height * 0.5f),
			ImGui::GetColorU32(EditorTheme::Token::Text), title);

		card.ContentX = cursor_x;
		card.Open = *open;
		card.HeaderHovered = header_hovered || action_hovered;
		return card;
	}

	/* 卡身内容与 EndCard 之间的补充间距（内容不全用属性行时，可自己排一行后调用） */
	inline void CardSeparator()
	{
		ImGui::Separator();
	}

	/* 收尾：算出卡高、把背景补到内容底下、把光标推到卡片下方 */
	inline void EndCard(const Card& card)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		ImDrawList* draw_list = ImGui::GetWindowDrawList();

		/* 卡底 = 内容底部 + 卡内留白；收起时卡就等于卡头本身 */
		const float content_bottom = ImGui::GetCursorScreenPos().y - style.ItemSpacing.y;
		const float header_bottom = card.Min.y + card.HeaderHeight;
		const float card_bottom = card.Open ? content_bottom + card.Pad : header_bottom;

		/* 卡背景向左右各外扩一档留白：卡身不缩进也能让字段离边框有距离，
		 * 而值列还能源源占满可用宽度（缩进会让值列少掉一块）。 */
		const ImVec2 card_lo(card.Min.x - card.Pad, card.Min.y);
		const ImVec2 card_far(card.Min.x + card.Width + card.Pad, card_bottom);
		const float rounding = style.FrameRounding;

		draw_list->ChannelsSetCurrent(0);

		/* 卡背景和边框用卡片自己的裁剪矩形（再跟窗口内框求交）。ImGui 的绘制裁剪会内缩半格内边距，
		 * 贴容器边缘的卡片最外几像素会被悄悄裁掉；换成卡片自己的矩形就能贴边、也不会画到滚动条上。 */
		const ImGuiWindow* window = ImGui::GetCurrentWindow();
		draw_list->PushClipRect(
			ImVec2(ImMax(card_lo.x, window->InnerRect.Min.x), ImMax(card_lo.y, window->InnerRect.Min.y)),
			ImVec2(ImMin(card_far.x, window->InnerRect.Max.x), ImMin(card_far.y, window->InnerRect.Max.y)),
			false);

		draw_list->AddRectFilled(card_lo, card_far,
			ImGui::GetColorU32(EditorTheme::Token::CardBg), rounding);

		/* 卡头比卡身亮一档、悬停再亮一档；收起时卡头就是整张卡，四角都要圆 */
		const ImVec4 header_fill = card.HeaderHovered
			? EditorTheme::Token::CardHeaderHovered : EditorTheme::Token::CardHeaderBg;
		const ImDrawFlags header_corners = (card_bottom > header_bottom + 0.5f)
			? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersAll;

		draw_list->AddRectFilled(card_lo, ImVec2(card_far.x, header_bottom),
			ImGui::GetColorU32(header_fill), rounding, header_corners);
		draw_list->AddRect(card_lo, card_far,
			ImGui::GetColorU32(EditorTheme::Token::Border), rounding);

		draw_list->PopClipRect();

		draw_list->ChannelsMerge();

		/* 卡与卡之间的间距：显式把光标推到卡片下方，免得和卡内留白互相纠缠 */
		ImGui::SetCursorScreenPos(ImVec2(card.Min.x, card_bottom + style.ItemSpacing.y));
	}
}
