#pragma once
/* 编辑器面板的通用版式：顶部行和分组卡片都在这定义一次，各面板只管描述内容。
 * 面板不画静态标题（名字由页签给出）。
 * 卡身里别用 ImGui::Columns / ChannelsSplit：卡片背景要等画完内容再补，通道不能套通道。 */

#include <string>
#include <cstring>
#include <imgui.h>
/* 卡片背景要用 ImGuiWindow::InnerRect（滚动条之外的可用区域），故需要内部头 */
#include <imgui_internal.h>
#include "EditorIcons.h"
#include "ImGui/EditorTheme.h"

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
	HeaderRow BeginHeaderRow(Icons::Id icon, float leading = 0.0f);

	/* 头部标题：标题字体 + 正文色，是面板里最重的一行文字。
	 * width > 0 时按它裁剪 —— 右端还有动作按钮时用它防止文字压上去。 */
	void DrawHeaderTitle(const HeaderRow& header, const std::string& title, float width = 0.0f);

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

	/* 只读单行文本，宽度由调用方限定（width）：超宽就用省略号收尾。
	 * layout_width > 0 时排版按它占位（横向滚动要有内容可滚），绘制仍只到 width；
	 * 不传就按 width 占位 —— 别用 PushClipRect + TextUnformatted：它只裁绘制不裁布局，
	 * 超宽文本会把面板撑出横向可滑区。 */
	void DrawClippedTextLine(const char* text, float width, float layout_width = 0.0f);

	/* 树节点行内容：图标 + 文本（marked 时带脏标记）。要在 TreeNodeEx 之后紧接着调用，
	 * TreeNode 的标签传空字符串 ""（不能传 "##"）；点击 / 拖拽 / 右键也都得在它后面立刻处理。
	 * clip_right > 0 时把文本裁到该 x 之前，给行右端留出尾随控件的位置（走 DrawClippedTextLine）。
	 * content_inset 是行右端控件占的宽：放不下时排版按"整段名字 + 它"占位，横向滑到底名字能整段露出来。 */
	void DrawTreeRowLabel(Icons::Id icon, const std::string& text, bool marked = false,
	                             ImVec4 icon_color = EditorTheme::Token::Text, float clip_right = 0.0f,
	                             float content_inset = 0.0f);

	/* ==================== 菜单行内容 ==================== */

	/* 带图标的菜单行（下拉条目）：整行命中 / 悬停高亮 / 点击收起都跟 MenuItem 一样，
	 * 只是文字前多一枚矢量图标；行宽要自己算（弹层是 AlwaysAutoResize，控件多宽弹层就多宽）。 */
	bool MenuItemWithIcon(Icons::Id icon, const char* label);

	/* 浮动菜单（右键 / 下拉）的统一版式：圆角 6、上下内边距 10、行距 4。
	 * ⚠️ 必须压在 Begin* 之前 —— 窗口的圆角与内边距在本帧第一次 Begin 时就锁定了，
	 * 压在窗口内部完全不生效（看起来像没改）。 */
	void PushMenuStyle();

	inline void PopMenuStyle()
	{
		ImGui::PopStyleVar(3);
	}

	/* 纯文字菜单行：版式和 MenuItemWithIcon 同一套（Selectable + 手绘文字），只是不画图标。
	 * 不用 ImGui::MenuItem —— 它会给勾选列和箭头列留位置，弹层右侧被白白撑宽。 */
	bool MenuItemText(const char* label, const char* shortcut = nullptr);

	/* 行尾勾选标记：行右缘固定列里的一枚矢量对勾（两笔画，Accent 色）——
	 * 切换行（MenuItemToggleWithIcon）与单选行（MenuItemSelectWithIcon）共用，
	 * 两处的"勾选列"逐像素一致；未勾选的行也保留同一列，位置不随状态跳。 */
	void DrawMenuRowCheck(ImDrawList* draw_list, const ImVec2& row_max, float center_y, float icon_size);

	/* 带图标和勾选的切换菜单行（Gizmos 显隐那一类）：整行命中 / 悬停高亮跟 MenuItemWithIcon 一样；
	 * 行右固定画一枚 Accent 色对勾，点击翻转 *value、不收起弹层。行高取控件档，行宽铺满弹层
	 * （SpanAvailWidth），各行的勾选列对齐在一条竖线上。 */
	bool MenuItemToggleWithIcon(Icons::Id icon, const char* label, bool* value);

	/* 三态复选框菜单行（"全选"这种聚合开关）：行右缘画三态复选框（全开 = 强调色实底 + 勾、
	 * 部分开 = 实底 + 横杠、全关 = 空框）。版式跟其它菜单行一样（控件档行高、整行命中、点击不收起）；
	 * 这一行自己不翻转任何值，只返回 clicked、由调用方决定。 */
	bool MenuItemTristateBox(bool all_on, bool none_on);

	/* 带图标的单选菜单行（"一组里选一个"的弹层条目）：外观跟 MenuItemToggleWithIcon 一样；
	 * 选中态由调用方传进来（本行不翻转值），点击不收起弹层。enabled = false 时整行禁用，
	 * 图标 / 文字 / 对勾跟着 BeginDisabled 一起变淡。 */
	bool MenuItemSelectWithIcon(Icons::Id icon, const char* label, bool selected, bool enabled = true);

	/* 菜单分区头（弹层里的分组标题）：不可点的小标题 —— 小一号大写字母 + 右侧一条细线。
	 * 行宽取内容区右缘（auto-resize 弹层从第二帧起就稳定了）。 */
	void MenuSectionHeader(const char* label);

	/* 带图标的子菜单行（「新建」下拉里的分组行）：外观 = 菜单行 + 右缘一个右向箭头；
	 * 行为照搬 ImGui BeginMenu 的垂直弹层路径（悬停展开、链式关闭、ChildMenu）。
	 * 只给弹层菜单用；键盘导航略过（这些菜单的键盘入口是搜索框）。 */
	bool BeginMenuWithIcon(Icons::Id icon, const char* label);

	/* 配对收尾（= EndMenu 的 EndPopup 部分；键盘 nav 的 Left 关闭逻辑不参与，从略） */
	inline void EndMenuWithIcon()
	{
		ImGui::EndPopup();
	}

	/* ==================== 可拖拽分隔条 ==================== */

	/* 左右两块之间的拖拽分隔条：按住左右拖，按 delta.x 改「左块占比」。
	 * ratio 由调用方持久化，钳在 [min_ratio, max_ratio] 之间；height 是命中区高度（给足整栏就行）。 */
	bool HorizontalSplitter(float& ratio, float total_width, float min_ratio, float max_ratio,
	                               float width, float height);

	/* ==================== 分组卡片 ==================== */

	/* 卡片背景往外扩的留白；容器内边距取这个值，卡片背景就贴住容器两边。
	 * 要用主题常量、别现算（按窗口内边距 × 0.6 现算的话，容器设过内边距后就不对了）。 */
	inline float CardPad()
	{
		return EditorTheme::Token::CardPad;
	}

	/* 卡片拖拽载荷的类型（内容 = 调用方给的字符串标识，比如组件名） */
	inline constexpr const char* kCardDragPayload = "HELIOS_CARD";

	struct Card
	{
		ImVec2 Min{ 0.0f, 0.0f };   /* 卡头行左上角（= 面板内容区原点） */
		float  Width{ 0.0f };       /* 内容区宽度 */
		float  Right{ 0.0f };       /* 卡右端 x */
		float  HeaderHeight{ 0.0f };
		float  ContentX{ 0.0f };    /* 卡头标题之后的 x */
		float  TitleEndX{ 0.0f };    /* 标题文字的右端 x（徽章从这里往后排） */
		float  HeaderActionX{ 0.0f }; /* 卡头右端动作位的左缘（徽章不许越过它） */
		float  Pad{ 0.0f };         /* 卡内留白（背景向左右各外扩这么多） */
		bool   Open{ true };
		bool   ActionClicked{ false };  /* 卡头右端的动作按钮被点了（调用方据此开菜单） */
		bool   HeaderHovered{ false };  /* 卡头整行（含动作位）是否悬停 —— 悬停要提亮 */
		bool   Reorderable{ false };    /* 卡头能拖走，整张卡能接住别的卡 */
		bool   Dragging{ false };       /* 本卡就是当前拖拽源（边框提亮） */
		ImGuiID DropId{ 0 };            /* 拖拽落点的 ID（卡自己的作用域内取） */
	};

	/* 拖拽落在卡上的结果（EndCard 返回）：Payload 是载荷里的字符串，只在当帧有效 */
	struct CardDrop
	{
		bool Received{ false };
		bool After{ false };            /* 落点在卡片下半：插到这张卡后面 */
		const char* Payload{ nullptr };
	};

	/* 折叠指示三角：朝下 = 展开、朝右 = 折叠；两个方向是同一个三角形转 90°
	 * （沿用 ImGui 箭头 0.866 / 0.75 的比例），免得切换方向时看起来变小。 */
	void DrawDisclosureArrow(ImDrawList* draw_list, const ImVec2& center, float size, bool open, ImU32 color);

	/* 开一张卡片：卡头（折叠箭头 + 可选图标 + 标题 + 右端动作）+ 卡身；动作按钮由卡片自己提交。
	 * 折叠状态存在窗口级存储里、键 = 标题（连同调用方 ID 作用域）；同名卡要调用方自己 PushID 区分。
	 * drag_payload 非空时卡头能拖走（内容就是这段字符串），点击折叠改成松手才判定，
	 * 拖过一段距离的松手不算点击。 */
	Card BeginCard(const char* title, Icons::Id title_icon = Icons::Id::None,
	                      Icons::Id action_icon = Icons::Id::None, const char* action_tooltip = nullptr,
	                      bool default_open = true, const char* drag_payload = nullptr);

	/* 卡头徽章：标题右侧一枚小圆角标签（如 Instance），只做标识、不抢卡头的点击。
	 * 必须在 BeginCard 之后、卡身之前调 —— 那时绘制通道还是卡头那一层。 */
	void DrawCardBadge(const Card& card, const char* text, const char* tooltip = nullptr);

	/* 卡身内容与 EndCard 之间的补充间距（内容不全用属性行时，可自己排一行后调用） */
	inline void CardSeparator()
	{
		ImGui::Separator();
	}

	/* 收尾：算出卡高、把背景补到内容底下、把光标推到卡片下方。
	 * 可重排的卡顺便接住拖过来的卡片，返回值告诉调用方是谁落到了哪儿。 */
	CardDrop EndCard(const Card& card);
}
