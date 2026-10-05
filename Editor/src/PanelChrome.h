#pragma once
/* 编辑器面板的通用版式（header-only）：顶部行和分组卡片都在这定义一次，各面板只管描述内容。
 * 面板不画静态标题（名字由页签给出）。
 * 卡身里别用 ImGui::Columns / ChannelsSplit：卡片背景要等画完内容再补，通道不能套通道。 */

#include <string>
#include <imgui.h>
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

	/* 开一行头部：画面板图标（None 则不画），并把光标留在标题起点。
	 * 标题由调用方画：静态用 DrawHeaderTitle，需要就地编辑就自己放输入框。 */
	inline HeaderRow BeginHeaderRow(Icons::Id icon)
	{
		const ImGuiStyle& style = ImGui::GetStyle();

		HeaderRow header;
		header.Min = ImGui::GetCursorScreenPos();
		header.Right = header.Min.x + ImGui::GetContentRegionAvail().x;
		header.Height = ImGui::GetFrameHeight();

		if (icon == Icons::Id::None)
		{
			header.TitleX = header.Min.x;
			return header;
		}

		/* 图标用标题字号：跟标题同一行时视觉重量才对得上 */
		ImGui::PushFont(EditorTheme::GetFonts().Title);
		const float icon_size = ImGui::GetFontSize();
		ImGui::PopFont();

		Icons::Draw(ImGui::GetWindowDrawList(), icon,
			ImVec2(header.Min.x + icon_size * 0.5f, header.Min.y + header.Height * 0.5f),
			icon_size, ImGui::GetColorU32(EditorTheme::Token::TextLabel));

		header.TitleX = header.Min.x + icon_size + style.ItemInnerSpacing.x;
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

	/* 树节点行内容：图标 + 文本（marked 时带脏标记）。要在 TreeNodeEx 之后紧接着调用，
	 * TreeNode 的标签传空字符串 ""（不能传 "##"）；点击 / 拖拽 / 右键也都得在它后面立刻处理。
	 * clip_right > 0 时把文本裁到该 x 之前，给行右端留出尾随控件的位置（走 DrawClippedTextLine）。 */
	inline void DrawTreeRowLabel(Icons::Id icon, const std::string& text, bool marked = false,
	                             ImVec4 icon_color = EditorTheme::Token::Text)
	{
		const ImGuiStyle& style = ImGui::GetStyle();

		/* 空标签的 TreeNode 末尾恰好落在「行首 + 一个字号」处（箭头右侧），
		 * 从这里再让出一点内边距，图标才不贴着箭头 */
		ImGui::SameLine(0.0f, style.ItemInnerSpacing.x + style.FramePadding.x);

		const float icon_size = ImGui::GetFontSize();
		ImGui::Dummy(ImVec2(icon_size, icon_size));

		const ImVec2 icon_min = ImGui::GetItemRectMin();
		const ImVec2 icon_max = ImGui::GetItemRectMax();
		Icons::DrawIcon(ImGui::GetWindowDrawList(), icon,
			ImVec2((icon_min.x + icon_max.x) * 0.5f, (icon_min.y + icon_max.y) * 0.5f),
			icon_size, ImGui::GetColorU32(icon_color));

		ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
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

	/* ==================== 分组卡片 ==================== */

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
		card.Pad = style.WindowPadding.x * 0.6f;

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

		draw_list->ChannelsMerge();

		/* 卡与卡之间的间距：显式把光标推到卡片下方，免得和卡内留白互相纠缠 */
		ImGui::SetCursorScreenPos(ImVec2(card.Min.x, card_bottom + style.ItemSpacing.y));
	}
}
