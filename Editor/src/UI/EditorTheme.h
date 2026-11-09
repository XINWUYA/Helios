#pragma once
/* Helios 编辑器主题模块（只依赖 imgui.h / ImGui 1.86 API，运行时状态在 EditorTheme.cpp）。设计原则：中性灰阶
 * 管"层级"、强调色只管"状态"；形状：停靠面板直角 → 控件 5px 圆角 → 浮层 8px；颜色 / 尺寸
 * 都先在 Token 定义。用法：首次 NewFrame 前调 SetupFonts + ApplyDark。 */

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace Helios::EditorTheme
{
    /* ==================== UI 缩放（高 DPI 自适应） ==================== */

    /* Windows 与 macOS 的 ImGui 坐标语义不同：
     * - macOS：GLFW 提供"点/像素"分离（FramebufferScale = 2），ImGui 空间 = 逻辑点；
     *   物理放大由渲染端乘 FramebufferScale 承担，设计值 1:1 进 ImGui（UiScale = 1）。
     * - Windows：GLFW 3.4 将进程设为 Per-Monitor DPI Aware，窗口 = 帧缓冲 = 物理像素、
     *   FramebufferScale = 1；要让 UI 呈现与系统缩放一致的大小，设计值必须显式乘系统缩放
     *   （UiScale = content scale，4K 200% → 2）。
     * 约定：一切"固定像素"（字体、Style、Token）按设计值书写，进入 ImGui 前统一乘 UiScale。
     *
     * 运行时可变：窗口在 DPI 不同的显示器之间移动时由 ImGuiLayer 更新（重建字体 + 重算样式）。
     * 所有换算一律"从设计值重算"（design × UiScale），不做累乘 —— 因此可重复调用。 */
    namespace Detail
    {
        float& UiScaleStorage();
        /* 当前主题序号（定义在 EditorTheme.cpp，同样为了跨模块只有一份） */
        int& ThemeIndexStorage();
    }

    /* 设计像素 → ImGui 单位的缩放系数（macOS = 1；Windows = 系统 content scale） */
    inline float UiScale()
    {
        return Detail::UiScaleStorage();
    }

    /* 设计像素 → ImGui 单位（= design_pixels × UiScale）的便捷换算：
     * Kernel 侧的编辑器 UI 组件用它；Editor 侧另有同义的 Scaled()（见 EditorCommon.h）。 */
    inline float Scaled(float design_pixels)
    {
        return design_pixels * UiScale();
    }

    /* ==================== Design Tokens（单一数据源） ==================== */

    namespace Token
    {
        /* 石墨蓝灰阶，由深到浅；用冷色明度差构建工作区层级 */
        inline const ImVec4 Neutral0 = ImVec4(0.067f, 0.086f, 0.114f, 1.00f); /* #11161D 工作区最深底 */
        inline const ImVec4 Neutral1 = ImVec4(0.090f, 0.118f, 0.153f, 1.00f); /* #171E27 未选中 Tab */
        inline const ImVec4 Neutral2 = ImVec4(0.114f, 0.149f, 0.192f, 1.00f); /* #1D2631 面板 / 窗口 */
        inline const ImVec4 Neutral3 = ImVec4(0.145f, 0.184f, 0.231f, 1.00f); /* #252F3B 菜单栏 / 表头 / 浮层 */
        inline const ImVec4 Neutral4 = ImVec4(0.180f, 0.224f, 0.275f, 1.00f); /* #2E3946 控件底色 */
        inline const ImVec4 Neutral5 = ImVec4(0.216f, 0.267f, 0.322f, 1.00f); /* #374452 控件悬停 */
        inline const ImVec4 Neutral6 = ImVec4(0.275f, 0.325f, 0.388f, 1.00f); /* #465363 控件按下 */
        inline const ImVec4 Neutral7 = ImVec4(0.365f, 0.420f, 0.482f, 1.00f); /* #5D6B7B 滚动条把手 / 滑块 */
        inline const ImVec4 Neutral8 = ImVec4(0.459f, 0.518f, 0.588f, 1.00f); /* #758496 滚动条拖拽中 */

        inline const ImVec4 Border    = ImVec4(0.263f, 0.314f, 0.376f, 0.78f); /* #435060 边框（全局唯一一档） */
        inline const ImVec4 Separator = ImVec4(0.169f, 0.212f, 0.267f, 0.94f); /* #2B3644 分隔线 */

        inline const ImVec4 Text    = ImVec4(0.910f, 0.933f, 0.957f, 1.00f); /* #E8EEF4 正文 */
        inline const ImVec4 TextDim = ImVec4(0.545f, 0.600f, 0.671f, 1.00f); /* #8B99AB 次要 / 禁用 */
        /* 属性行标签：比正文弱、比禁用强 —— 标签是"说明"，值才是内容 */
        inline const ImVec4 TextLabel = ImVec4(0.765f, 0.808f, 0.863f, 1.00f); /* #C3CEDC */
        /* 错误提示：只用于"当下就不对"的输入（如就地改名的非法名字），
         * 不参与状态色带（选中 / 悬停是 Accent 家族），也别拿它当装饰色 */
        inline const ImVec4 Danger    = ImVec4(0.851f, 0.482f, 0.459f, 1.00f); /* #D97B75 */
        /* 警告提示：介于正文与错误之间的"需要注意"语义（日志窗口的 WARN 级标色） */
        inline const ImVec4 Warning   = ImVec4(0.851f, 0.702f, 0.420f, 1.00f); /* #D9B36B */

        /* 运行态标识：Play 中的视口描边与工具栏状态色。色相刻意避开 Warning（琥珀）、
         * Danger（红）与各主题的 Accent，且不随主题切换 —— 它说的是"正在运行" */
        inline const ImVec4 Playing   = ImVec4(0.302f, 0.827f, 0.478f, 1.00f); /* #4DD37A */

        /* 强调色一族：运行时可变（工具栏切换主题），所以定义收口在 EditorTheme.cpp ——
         * Windows 上 Kernel.dll 与 Editor.exe 各有一份头文件里的 inline 变量，改一处另一处看不到。
         * 只用于选中 / 激活 / 焦点 / 拖拽，不作为装饰色铺满界面 */
        extern ImVec4 Accent;
        extern ImVec4 AccentHover;
        extern ImVec4 AccentDown;

        /* 图标侧的派生态（换主题时由 RefreshAccentTokens 从 Accent 折算）：
         * Ink 主色、Tint 深底淡底、OnLight 亮底深描边、Pale 亮底极淡填充；
         * 带 Alt 的是副色（色轮转 150° 的对比色），语义相同。 */
        extern ImVec4 AccentInk;
        extern ImVec4 AccentTint;
        extern ImVec4 AccentOnLight;
        extern ImVec4 AccentPale;
        extern ImVec4 AccentAlt;
        extern ImVec4 AccentAltDeep;
        extern ImVec4 AccentAltTint;
        extern ImVec4 AccentAltPale;

        /* 分量色：X/Y/Z 的身份标识（红 / 绿 / 蓝），只出现在「分量重置按钮」和「视图指示器」两处
         * （共用 AxisGlyph.h 的字母笔画）；深色界面用降饱和版本。W 没有约定色，用中性灰。 */
        inline const ImVec4 AxisX      = ImVec4(0.784f, 0.396f, 0.373f, 1.00f); /* #C8655F */
        inline const ImVec4 AxisXHover = ImVec4(0.851f, 0.482f, 0.459f, 1.00f); /* #D97B75 */
        inline const ImVec4 AxisY      = ImVec4(0.435f, 0.667f, 0.353f, 1.00f); /* #6FAA5A */
        inline const ImVec4 AxisYHover = ImVec4(0.522f, 0.749f, 0.439f, 1.00f); /* #85BF70 */
        inline const ImVec4 AxisZ      = ImVec4(0.353f, 0.549f, 0.831f, 1.00f); /* #5A8CD4 */
        inline const ImVec4 AxisZHover = ImVec4(0.443f, 0.639f, 0.914f, 1.00f); /* #71A3E9 */

        /* 组件卡：面板(Neutral2) < 卡身(Neutral3) < 卡头(Neutral4)，用明度递增表达"抬起" */
        inline const ImVec4 CardBg            = Neutral3;
        inline const ImVec4 CardHeaderBg      = Neutral4;
        inline const ImVec4 CardHeaderHovered = Neutral5;

        /* 滑动条（细线 + 圆点，ImGuiExt::DrawDotSliderFloat）：
         * 细轨用中性灰；点亮段与圆点一起表示"当前值"——当前值在强调色白名单里，
         * 故用 Accent；悬停 / 拖动时整条再亮一档，状态的差异只在这三个颜色里。 */
        inline const ImVec4 SliderTrack = Neutral6;      /* #465363 细轨（值右侧那一段） */
        /* 点亮段与圆点跟着主题走，值由 RefreshAccentTokens 落定 */
        extern ImVec4 SliderValue;
        extern ImVec4 SliderHover;

        inline const ImVec4 Clear   = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        inline const ImVec4 White06 = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
        inline const ImVec4 White12 = ImVec4(1.0f, 1.0f, 1.0f, 0.12f);

        /* ---- 尺寸 ----
         * 每个尺寸都是"设计值常量（*Design）+ 运行时值"：设计值按逻辑像素书写，
         * 运行时值 = 设计值 × UiScale，由 ApplyUiScaleToTokens 在 SetupFonts 内落定。
         * 使用方只读运行时值（如 Token::PropertyLabelWidth），不要再写 *Design。 */

        /* 属性面板标签列宽：全面板共用一档（各卡字段左边界对齐）。覆盖最长的字段名（PrefilterMipLevels）
         * 和最长的材质参数名（u_DisplacementTexture）；行内按实际文本做下限保护，更长名字只撑宽自己那一行。 */
        inline constexpr float PropertyLabelWidthDesign = 124.0f;

        /* 属性行的垂直内边距（FramePadding.y）：行高 = 字体高 + 2×这一档。
         * 行内文字（标签与值）统一取同一档基线，竖直居中、全面板同一节奏。
         * 取值手感：0 = 紧贴文字（过于发挤）；5（全局档）= 回到 24px 的老行高。 */
        inline constexpr float PropertyRowPadYDesign = 3.0f;

        /* 分组卡片背景的外扩留白（PanelChrome::CardPad）：容器内边距取这个值就贴住容器两边。
         * 注意：必须是常量（早先按"当前窗口内边距 × 0.6"现算，容器设过内边距后就永远差一截）。 */
        inline constexpr float CardPadDesign = 6.0f;

        /* 滑动条（细线 + 圆点）：线要细、点要粗，"点在线上"才有主次。
         * 圆点直径同时是拖动行程的基准（控件把它压给 GrabMinSize），改这里两端会等量内收。 */
        inline constexpr float SliderTrackThicknessDesign = 3.0f;
        inline constexpr float SliderDotRadiusDesign      = 5.0f;

        /* 运行时值 = 设计值 × UiScale（ApplyUiScaleToTokens 落定，使用方只读） */
        extern float PropertyLabelWidth;
        extern float PropertyRowPadY;
        extern float CardPad;
        extern float SliderTrackThickness;
        extern float SliderDotRadius;
    }

    /* 按 UiScale 落定 Token 尺寸（SetupFonts / 跨屏重建时调用；从设计值重算而非累乘，
     * 因此可重复调用也安全） */
    inline void ApplyUiScaleToTokens(float ui_scale)
    {
        Token::PropertyLabelWidth   = Token::PropertyLabelWidthDesign * ui_scale;
        Token::PropertyRowPadY      = Token::PropertyRowPadYDesign * ui_scale;
        Token::CardPad              = Token::CardPadDesign * ui_scale;
        Token::SliderTrackThickness = Token::SliderTrackThicknessDesign * ui_scale;
        Token::SliderDotRadius      = Token::SliderDotRadiusDesign * ui_scale;
    }

    inline ImVec4 WithAlpha(const ImVec4& color, float alpha)
    {
        return ImVec4(color.x, color.y, color.z, alpha);
    }

    /* 按比例压暗 / 提亮（保持色相）：派生态都从强调色算，不另存色值 */
    inline ImVec4 ScaleRgb(const ImVec4& color, float factor)
    {
        return ImVec4(color.x * factor, color.y * factor, color.z * factor, color.w);
    }

    inline ImVec4 MixRgb(const ImVec4& from, const ImVec4& to, float t)
    {
        return ImVec4(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t,
            from.z + (to.z - from.z) * t, 1.0f);
    }

    /* 辅色太深时按比例混白、提到能在深色底上描边（色相不变）：
     * 主题表里的深紫 / 靛青 / 石墨这类副色直接用会看不见 */
    inline ImVec4 LiftForDark(const ImVec4& color, float min_level)
    {
        const float max_c = std::max(color.x, std::max(color.y, color.z));
        if (max_c >= min_level)
            return color;

        const float t = (min_level - max_c) / std::max(1.0f - max_c, 1e-4f);
        return MixRgb(color, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), t);
    }

    /* 已选中行被悬停时的填充色：保住强调色底、再亮一档。ImGui 对「悬停 + 选中」画的也是
     * HeaderHovered（会把选中色整个盖掉）；选中行的调用点要显式 PushStyleColor 顶住。
     * 值随主题刷新（见 RefreshAccentTokens），不要手写。 */
    extern ImVec4 RowHoverSelected;

    /* ==================== 形状语言（Style 数值） ==================== */

    inline void ApplyMetrics(ImGuiStyle& style)
    {
        style.Alpha          = 1.0f;
        style.DisabledAlpha  = 0.45f;             /* 禁用态更明确 */

        /* ---- 层级：面板直角贴停靠，浮层用圆角表达"悬浮" ---- */
        style.WindowPadding        = ImVec2(12.0f, 9.0f);
        style.WindowRounding       = 0.0f;        /* 停靠面板保持直角（编辑器习惯） */
        style.WindowBorderSize     = 0.0f;        /* 边框交给背景明度差，不用描边 */
        style.WindowMinSize        = ImVec2(200.0f, 120.0f);
        style.WindowTitleAlign     = ImVec2(0.0f, 0.5f);
        style.WindowMenuButtonPosition = ImGuiDir_None; /* 不画折叠/停靠按钮，页签栏右端保持干净 */

        style.ChildRounding        = 5.0f;
        style.ChildBorderSize      = 1.0f;
        style.PopupRounding        = 8.0f;        /* 浮层 = 最大圆角 = 层级提示 */
        style.PopupBorderSize      = 1.0f;

        /* ---- 控件：统一的 5px 圆角 + 一档细边框 ---- */
        style.FramePadding         = ImVec2(8.0f, 5.0f);  /* 控件更舒展，约 24px 行高 */
        style.FrameRounding        = 5.0f;
        style.FrameBorderSize      = 1.0f;        /* 输入框有清晰边界 */
        style.ItemSpacing          = ImVec2(8.0f, 7.0f);  /* 行距呼吸感 */
        style.ItemInnerSpacing     = ImVec2(7.0f, 5.0f);
        style.CellPadding          = ImVec2(6.0f, 3.0f);
        style.TouchExtraPadding    = ImVec2(0.0f, 0.0f);

        /* ---- 列表 / 树 ---- */
        style.IndentSpacing        = 18.0f;       /* 树层级更紧凑 */
        style.ColumnsMinSpacing    = 8.0f;
        style.SelectableTextAlign  = ImVec2(0.0f, 0.5f); /* 行文本垂直居中（如不习惯改回 0,0） */

        /* ---- 滚动条：细 + 圆 ---- */
        style.ScrollbarSize        = 10.0f;
        style.ScrollbarRounding    = 6.0f;
        style.GrabMinSize          = 12.0f;
        style.GrabRounding         = 4.0f;

        /* ---- Tab ---- */
        style.TabRounding          = 5.0f;
        style.TabBorderSize        = 0.0f;        /* Tab 不画描边，靠底色区分 */
        style.TabMinWidthForCloseButton = 0.0f;
        style.ColorButtonPosition  = ImGuiDir_Right;
        style.ButtonTextAlign      = ImVec2(0.5f, 0.5f);

        /* ---- 抗锯齿质量 ---- */
        style.AntiAliasedLines     = true;
        style.AntiAliasedLinesUseTex = true;
        style.AntiAliasedFill      = true;
        style.CurveTessellationTol = 1.25f;
        style.CircleTessellationMaxError = 0.20f; /* 圆角更细腻 */
    }

    /* ==================== Helios Dark 配色 ==================== */

    /* 中性灰阶与语义色：跟主题无关，只在 ApplyDark 里整表写一次 */
    inline void ApplyBaseColors(ImGuiStyle& style)
    {
        ImVec4* c = style.Colors;

        c[ImGuiCol_Text]                   = Token::Text;
        c[ImGuiCol_TextDisabled]           = Token::TextDim;

        c[ImGuiCol_WindowBg]               = Token::Neutral2;               /* 面板/窗口背景 */
        c[ImGuiCol_ChildBg]                = Token::Clear;
        c[ImGuiCol_PopupBg]                = WithAlpha(Token::Neutral3, 0.98f); /* 浮层比面板亮一档 */

        c[ImGuiCol_Border]                 = Token::Border;
        c[ImGuiCol_BorderShadow]           = Token::Clear;

        c[ImGuiCol_FrameBg]                = Token::Neutral4;               /* 输入框 */
        c[ImGuiCol_FrameBgHovered]         = Token::Neutral5;
        c[ImGuiCol_FrameBgActive]          = Token::Neutral6;

        /* 注意：停靠模式下这两个颜色画的是"Tab 条"（imgui.cpp: DockNodeUpdate）。
         * 亮度单调性：Tab 条必须比面板/Tab 暗，否则产生下凹感。 */
        c[ImGuiCol_TitleBg]                = Token::Neutral0;               /* 未聚焦节点 Tab 条（比面板暗两档） */
        c[ImGuiCol_TitleBgActive]          = Token::Neutral1;               /* 聚焦节点 Tab 条（比面板暗一档的"轨道"） */
        c[ImGuiCol_TitleBgCollapsed]       = WithAlpha(Token::Neutral0, 0.75f);
        c[ImGuiCol_MenuBarBg]              = Token::Neutral3;

        c[ImGuiCol_ScrollbarBg]            = Token::Clear;                  /* 滚动条无轨道 */
        c[ImGuiCol_ScrollbarGrab]          = WithAlpha(Token::Neutral7, 0.55f);
        c[ImGuiCol_ScrollbarGrabHovered]   = WithAlpha(Token::Neutral7, 0.85f);
        c[ImGuiCol_ScrollbarGrabActive]    = Token::Neutral8;

        c[ImGuiCol_SliderGrab]             = Token::Neutral7;

        c[ImGuiCol_Button]                 = WithAlpha(Token::Neutral5, 0.90f);
        c[ImGuiCol_ButtonHovered]          = Token::Neutral5;
        c[ImGuiCol_ButtonActive]           = Token::Neutral6;

        /* 列表行：悬停 = 中性提亮（White 12%）。选中 / 按下在 ApplyAccentColors。
         * 「悬停 + 选中」被 ImGui 也画成 HeaderHovered —— 那个组合由选中行的调用点
         * 推 RowHoverSelected 顶住（见它的注释）。 */
        c[ImGuiCol_HeaderHovered]          = Token::White12;

        c[ImGuiCol_Separator]              = Token::Separator;

        /* Tab：亮度单调递增 —— 条(Neutral1) < 未激活(Neutral2) < 激活(Neutral3)。
         * 激活项永远最亮，避免"亮的条 + 暗的 Tab"反向对比产生的下凹感。 */
        c[ImGuiCol_Tab]                    = Token::Neutral2;               /* 未激活 = 面板色，浮在条上 */
        c[ImGuiCol_TabHovered]             = Token::Neutral4;
        c[ImGuiCol_TabActive]              = Token::Neutral3;               /* 激活 = 全条最亮（抬起） */
        c[ImGuiCol_TabUnfocused]           = Token::Neutral1;               /* 未聚焦节点：条为 N0，Tab 亮一档 */
        c[ImGuiCol_TabUnfocusedActive]     = WithAlpha(Token::Neutral3, 0.80f);

        c[ImGuiCol_DockingEmptyBg]         = Token::Neutral0;               /* 停靠空区 = 最深底 */

        c[ImGuiCol_PlotLines]              = Token::Neutral7;

        c[ImGuiCol_TableHeaderBg]          = Token::Neutral3;
        c[ImGuiCol_TableBorderStrong]      = ImVec4(0.227f, 0.251f, 0.282f, 1.00f);
        c[ImGuiCol_TableBorderLight]       = ImVec4(0.165f, 0.184f, 0.212f, 1.00f);
        c[ImGuiCol_TableRowBg]             = Token::Clear;
        c[ImGuiCol_TableRowBgAlt]          = ImVec4(1.0f, 1.0f, 1.0f, 0.03f);

        c[ImGuiCol_NavWindowingHighlight]  = ImVec4(1.0f, 1.0f, 1.0f, 0.70f);
        c[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.0f, 0.0f, 0.0f, 0.35f);
        c[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.03f, 0.03f, 0.04f, 0.55f);
    }

    /* 强调色一族：切主题时只重刷这些（见 SetTheme），其余颜色与尺寸原样留着 */
    inline void ApplyAccentColors(ImGuiStyle& style)
    {
        ImVec4* c = style.Colors;

        c[ImGuiCol_CheckMark]              = Token::Accent;
        c[ImGuiCol_SliderGrabActive]       = Token::Accent;                 /* 拖拽滑块 = 强调 */

        /* 列表行：选中 = 强调色浸染 40%，按下 = 55% */
        c[ImGuiCol_Header]                 = WithAlpha(Token::Accent, 0.40f);
        c[ImGuiCol_HeaderActive]           = WithAlpha(Token::Accent, 0.55f);

        c[ImGuiCol_SeparatorHovered]       = WithAlpha(Token::Accent, 0.60f);
        c[ImGuiCol_SeparatorActive]        = Token::Accent;

        c[ImGuiCol_ResizeGrip]             = WithAlpha(Token::Accent, 0.12f);
        c[ImGuiCol_ResizeGripHovered]      = WithAlpha(Token::Accent, 0.45f);
        c[ImGuiCol_ResizeGripActive]       = WithAlpha(Token::Accent, 0.80f);

        c[ImGuiCol_DockingPreview]         = WithAlpha(Token::Accent, 0.55f);

        c[ImGuiCol_PlotLinesHovered]       = Token::AccentHover;
        c[ImGuiCol_PlotHistogram]          = WithAlpha(Token::Accent, 0.90f);
        c[ImGuiCol_PlotHistogramHovered]   = Token::AccentHover;

        c[ImGuiCol_TextSelectedBg]         = WithAlpha(Token::Accent, 0.38f);
        c[ImGuiCol_DragDropTarget]         = WithAlpha(Token::Accent, 0.92f);
        c[ImGuiCol_NavHighlight]           = Token::Accent;
    }

    inline void ApplyDark()
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style = ImGuiStyle(); /* 先回到官方默认值，避免残留旧配色 */
        ApplyMetrics(style);
        /* 设计像素 → ImGui 单位：一次性缩放内置尺寸（padding / rounding / spacing 等）。
         * macOS 与 Windows 100% 下 UiScale = 1 是 no-op，高 DPI 时控件与字体同步放大；
         * 必须在任何 Push 样式栈之前调用。ImFloor 取整，非整数缩放下个别值少 0~1px。 */
        style.ScaleAllSizes(UiScale());

        ApplyBaseColors(style);
        ApplyAccentColors(style);
    }

    /* ==================== 主题预设（菜单栏切换） ==================== */

    /* 一套主题两个色：亮的那档当强调色（深色面板上才看得见），另一档当辅色给图标 / 装饰。
     * 悬停 / 按下 / 各种淡色全由 RefreshAccentTokens 折算 —— 加主题只动这张表 */
    struct ThemePreset
    {
        const char* Name;
        ImVec4 Accent;
        ImVec4 Alt;
    };

    inline const ThemePreset kThemePresets[] =
    {
        /* 皇家金 #F2C14E + 香槟金 #F7E7CE */ { "Royal Gold",    ImVec4(0.949f, 0.757f, 0.306f, 1.0f),
                                        ImVec4(0.969f, 0.906f, 0.808f, 1.0f) },
        /* 梦幻紫 #A69EFF + #302A63 */ { "Dream Violet",  ImVec4(0.651f, 0.620f, 1.000f, 1.0f),
                                        ImVec4(0.188f, 0.165f, 0.388f, 1.0f) },
        /* 深海薄荷 #0F4C81 + #2EC4B6 */ { "Deep Sea Mint", ImVec4(0.180f, 0.769f, 0.714f, 1.0f),
                                        ImVec4(0.059f, 0.298f, 0.506f, 1.0f) },
        /* 松柏米白 #1F6F50 + #F4EBDD */ { "Pine Cream",    ImVec4(0.957f, 0.922f, 0.867f, 1.0f),
                                        ImVec4(0.122f, 0.435f, 0.314f, 1.0f) },
        /* 靛青珊瑚 #2E3A8C + #F26B5B */ { "Indigo Coral",  ImVec4(0.949f, 0.420f, 0.357f, 1.0f),
                                        ImVec4(0.180f, 0.227f, 0.549f, 1.0f) },
        /* 石墨雾蓝 #3A3A3A + #A8C7E8 */ { "Graphite Blue", ImVec4(0.659f, 0.780f, 0.910f, 1.0f),
                                        ImVec4(0.227f, 0.227f, 0.227f, 1.0f) },
    };

    inline int ThemeCount()
    {
        return static_cast<int>(sizeof(kThemePresets) / sizeof(kThemePresets[0]));
    }

    inline const ThemePreset& GetTheme(int index)
    {
        return kThemePresets[(index >= 0 && index < ThemeCount()) ? index : 0];
    }

    /* 当前主题序号（越界一律落到 0） */
    inline int ActiveTheme()
    {
        return Detail::ThemeIndexStorage();
    }

    /* 由当前主题的一对色折算全部派生态：切主题与启动初始化共用一套算术 */
    inline void RefreshAccentTokens()
    {
        const ThemePreset& preset = GetTheme(Detail::ThemeIndexStorage());

        Token::Accent      = preset.Accent;
        Token::AccentHover = MixRgb(preset.Accent, ImVec4(1.0f, 1.0f, 1.0f, 1.0f), 0.22f);
        Token::AccentDown  = ScaleRgb(preset.Accent, 0.84f);

        Token::AccentInk     = ScaleRgb(Token::Accent, 0.82f);  /* 图标主色，深一档 */
        Token::AccentTint    = ScaleRgb(Token::Accent, 0.28f);  /* 深色画布上的淡底 */
        Token::AccentOnLight = ScaleRgb(Token::Accent, 0.68f);  /* 亮底上的深描边 */
        Token::AccentPale    = MixRgb(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), Token::Accent, 0.18f);

        /* 辅色：深色档先提到能在暗底上描边，再由它折出淡色变体 */
        Token::AccentAlt     = LiftForDark(preset.Alt, 0.55f);
        Token::AccentAltDeep = ScaleRgb(Token::AccentAlt, 0.72f);
        Token::AccentAltTint = ScaleRgb(Token::AccentAlt, 0.30f);
        Token::AccentAltPale = MixRgb(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), Token::AccentAlt, 0.12f);

        Token::SliderValue = Token::Accent;
        Token::SliderHover = Token::AccentHover;
        RowHoverSelected   = WithAlpha(Token::Accent, 0.50f);
    }

    /* 切主题：写 Token 并只重刷强调色那几项 —— 不重置整张样式表，切换时不会闪 */
    inline void SetTheme(int index)
    {
        if (index < 0 || index >= ThemeCount())
            index = 0;

        Detail::ThemeIndexStorage() = index;
        RefreshAccentTokens();

        if (ImGui::GetCurrentContext() != nullptr)
            ApplyAccentColors(ImGui::GetStyle());
    }

    /* ==================== 字体系统 ==================== */

    /* 两个要点：中文要显式传 GlyphRanges（默认只含 Latin，中文会渲染成 '?'）；Retina 按 dpi_scale
     * 倍光栅化 + font->Scale 缩回逻辑尺寸（2x 屏原生渲染、不糊）。
     * 字体组合：OpenSans（拉丁）+ msyh（常用中文 2500 字），先到先得、msyh 用 MergeMode 补缺。 */
    struct EditorFonts
    {
        ImFont* Regular = nullptr; /* 正文（逻辑 14px） */
        ImFont* Title   = nullptr; /* 面板标题（逻辑 16px） */
        ImFont* Display = nullptr; /* 启动页大字（逻辑 28px，只有拉丁字形） */
    };

    /* 全局字体存取：各面板用 GetFonts().Title 切换标题字体 */
    namespace Detail
    {
        EditorFonts& FontsStorage();
    }

    inline const EditorFonts& GetFonts()
    {
        return Detail::FontsStorage();
    }

    inline ImFont* AddMergedFont(ImGuiIO& io,
                                 const std::string& latin_ttf,
                                 const std::string& cjk_ttf,
                                 float logical_size,
                                 float dpi_scale)
    {
        ImFontConfig latin_cfg;
        latin_cfg.OversampleH = 2;
        latin_cfg.OversampleV = 1;

        ImFont* font = io.Fonts->AddFontFromFileTTF(latin_ttf.c_str(), logical_size * dpi_scale,
                                                    &latin_cfg, io.Fonts->GetGlyphRangesDefault());
        if (font == nullptr)
            return nullptr;

        if (cjk_ttf.size() > 0)
        {
            ImFontConfig cjk_cfg;
            cjk_cfg.OversampleH = 2;
            cjk_cfg.OversampleV = 1;
            cjk_cfg.MergeMode   = true; /* 仅补入基础字体缺失的字形（中文/标点） */
            io.Fonts->AddFontFromFileTTF(cjk_ttf.c_str(), logical_size * dpi_scale, &cjk_cfg,
                                         io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        }

        font->Scale = UiScale() / dpi_scale; /* 光栅化保留 dpi 倍细节：
                                              * macOS 缩回逻辑尺寸（Scale = 1/dpi，物理放大由渲染端乘 FramebufferScale 承担）；
                                              * Windows 不缩回（显示 = 设计值 × UiScale 个物理像素，直接就是最终尺寸）。 */
        return font;
    }

    /* 注意：dpi_scale 要在窗口创建后、字体构建前拿（glfwGetWindowContentScale）。
     * 需要图标字体的话：把 ttf 放到 font_dir 下、把 icon_ttf 文件名传进来。 */
    inline EditorFonts SetupFonts(ImGuiIO& io,
                                  const std::string& font_dir,
                                  float dpi_scale,
                                  const std::string& icon_ttf = std::string())
    {
        EditorFonts fonts;

        /* 先落定 UI 缩放与 Token 尺寸：字体、样式（ApplyDark）、各面板常量都按它折算。
         * Windows 用系统缩放；macOS 恒 1（物理放大在 framebuffer scale 上，见 UiScale 说明）。
         * 跨屏重建时会再次进来（先 Clear 图集），因此这里必须"从设计值重算"，不能累乘。 */
#if defined(_WIN32)
        const float ui_scale = (dpi_scale > 0.0f) ? dpi_scale : 1.0f;
#else
        const float ui_scale = 1.0f;
#endif
        Detail::UiScaleStorage() = ui_scale;
        ApplyUiScaleToTokens(ui_scale);

        const std::string latin_regular = font_dir + "/opensans/OpenSans-Regular.ttf";
        const std::string latin_semibold = font_dir + "/opensans/OpenSans-SemiBold.ttf";
        const std::string cjk = font_dir + "/msyh.ttf";

        fonts.Regular = AddMergedFont(io, latin_regular, cjk, 14.0f, dpi_scale);
        fonts.Title   = AddMergedFont(io, latin_semibold, cjk, 16.0f, dpi_scale);

        /* 启动页的字样只用拉丁：28px 再合并两千多个中文字形，字体图集会翻好几倍 */
        fonts.Display = AddMergedFont(io, latin_semibold, std::string(), 28.0f, dpi_scale);

        if (fonts.Regular == nullptr) /* 拉丁字体缺失时退化为纯中文单字体 */
            fonts.Regular = AddMergedFont(io, cjk, std::string(), 14.0f, dpi_scale);

        /* 图标字体（私用区 0xE000-0xF8FF），合并进正文字体 */
        if (fonts.Regular != nullptr && icon_ttf.size() > 0)
        {
            static const ImWchar icon_ranges[] = { 0xE000, 0xF8FF, 0 };
            ImFontConfig icon_cfg;
            icon_cfg.MergeMode         = true;
            icon_cfg.PixelSnapH        = true;
            icon_cfg.GlyphMinAdvanceX  = 14.0f * dpi_scale; /* 图标按方正单元格对齐 */
            io.Fonts->AddFontFromFileTTF((font_dir + "/" + icon_ttf).c_str(), 14.0f * dpi_scale,
                                         &icon_cfg, icon_ranges);
        }

        io.FontDefault = fonts.Regular;
        Detail::FontsStorage() = fonts;
        return fonts;
    }

    /* ==================== 主 DockSpace 宿主窗口 ==================== */

    /* 用法（MainEditorLayer::ShowMenuUI）：用 PushDockHostBackground() 包住宿主窗口的 Begin/End，
     * 宿主背景压暗到 Neutral0，停靠面板就像浮在更深的画布上；不影响其它 Layer 的面板窗口。 */
    inline void PushDockHostBackground()
    {
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Token::Neutral0);
    }

    inline void PopDockHostBackground()
    {
        ImGui::PopStyleColor();
    }
}
