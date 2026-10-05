#pragma once
/* Helios 编辑器主题模块（header-only，只依赖 imgui.h / ImGui 1.86 API）。设计原则：中性灰阶
 * 管"层级"、强调色只管"状态"；形状：停靠面板直角 → 控件 5px 圆角 → 浮层 8px；颜色 / 尺寸
 * 都先在 Token 定义。用法：首次 NewFrame 前调 SetupFonts + ApplyDark。 */

#include <imgui.h>

#include <string>

namespace Helios::EditorTheme
{
    /* ==================== Design Tokens（单一数据源） ==================== */

    namespace Token
    {
        /* 中性灰阶（冷调），由深到浅 —— 用明度差表达层级，不用彩色 */
        inline const ImVec4 Neutral0 = ImVec4(0.078f, 0.086f, 0.102f, 1.00f); /* #14161A 工作区最深底 */
        inline const ImVec4 Neutral1 = ImVec4(0.098f, 0.110f, 0.125f, 1.00f); /* #191C20 未选中 Tab */
        inline const ImVec4 Neutral2 = ImVec4(0.118f, 0.129f, 0.149f, 1.00f); /* #1E2126 面板 / 窗口 */
        inline const ImVec4 Neutral3 = ImVec4(0.149f, 0.165f, 0.192f, 1.00f); /* #262A31 菜单栏 / 表头 / 浮层 */
        inline const ImVec4 Neutral4 = ImVec4(0.173f, 0.192f, 0.220f, 1.00f); /* #2C3138 控件底色 */
        inline const ImVec4 Neutral5 = ImVec4(0.204f, 0.227f, 0.263f, 1.00f); /* #343A43 控件悬停 */
        inline const ImVec4 Neutral6 = ImVec4(0.243f, 0.271f, 0.314f, 1.00f); /* #3E4550 控件按下 */
        inline const ImVec4 Neutral7 = ImVec4(0.290f, 0.322f, 0.365f, 1.00f); /* #4A525D 滚动条把手 / 滑块 */
        inline const ImVec4 Neutral8 = ImVec4(0.360f, 0.400f, 0.450f, 1.00f); /* #5C6673 滚动条拖拽中 */

        inline const ImVec4 Border    = ImVec4(0.227f, 0.251f, 0.282f, 0.72f); /* #3A4048 边框（全局唯一一档） */
        inline const ImVec4 Separator = ImVec4(0.165f, 0.184f, 0.212f, 0.90f); /* #2A2F36 分隔线 */

        inline const ImVec4 Text    = ImVec4(0.902f, 0.914f, 0.929f, 1.00f); /* #E6E9ED 正文 */
        inline const ImVec4 TextDim = ImVec4(0.431f, 0.463f, 0.506f, 1.00f); /* #6E7681 次要 / 禁用 */
        /* 属性行标签：比正文弱、比禁用强 —— 标签是"说明"，值才是内容 */
        inline const ImVec4 TextLabel = ImVec4(0.725f, 0.753f, 0.792f, 1.00f); /* #B9C0CA */

        /* 强调色：只允许出现在 选中 / 激活 / 当前值 / 焦点 / 拖拽预览 */
        inline const ImVec4 Accent      = ImVec4(0.298f, 0.604f, 1.000f, 1.00f); /* #4C9AFF */
        inline const ImVec4 AccentHover = ImVec4(0.435f, 0.682f, 1.000f, 1.00f); /* #6FAEFF */
        inline const ImVec4 AccentDown  = ImVec4(0.239f, 0.518f, 0.910f, 1.00f); /* #3D84E8 */

        /* 分量色：X/Y/Z 的身份标识（业界约定 红/绿/蓝），与"状态"无关，
         * 因此只出现在「分量重置按钮」这一处；深色界面下取降饱和版本，避免抢眼。
         * 四分量（W）没有约定俗成的颜色，用中性灰，不让它冒充分量语义。 */
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

        inline const ImVec4 Clear   = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        inline const ImVec4 White06 = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
        inline const ImVec4 White12 = ImVec4(1.0f, 1.0f, 1.0f, 0.12f);

        /* ---- 尺寸 ---- */

        /* 属性面板标签列宽：全面板共用一档，所有组件卡的字段左边界因此对齐。
         * 取值覆盖当前注册表里最长的字段名（PrefilterMipLevels）；行内仍会按实际
         * 文本做一次下限保护，将来出现更长的名字只会撑宽自己那一行而不会重叠。 */
        inline constexpr float PropertyLabelWidth = 108.0f;
    }

    inline ImVec4 WithAlpha(const ImVec4& color, float alpha)
    {
        return ImVec4(color.x, color.y, color.z, alpha);
    }

    /* ==================== 形状语言（Style 数值） ==================== */

    inline void ApplyMetrics(ImGuiStyle& style)
    {
        style.Alpha          = 1.0f;
        style.DisabledAlpha  = 0.45f;             /* 禁用态更明确 */

        /* ---- 层级：面板直角贴停靠，浮层用圆角表达"悬浮" ---- */
        style.WindowPadding        = ImVec2(10.0f, 8.0f);
        style.WindowRounding       = 0.0f;        /* 停靠面板保持直角（编辑器习惯） */
        style.WindowBorderSize     = 0.0f;        /* 边框交给背景明度差，不用描边 */
        style.WindowMinSize        = ImVec2(200.0f, 120.0f);
        style.WindowTitleAlign     = ImVec2(0.0f, 0.5f);
        style.WindowMenuButtonPosition = ImGuiDir_Right; /* 折叠/停靠按钮放右侧，避免误点 */

        style.ChildRounding        = 4.0f;
        style.ChildBorderSize      = 1.0f;
        style.PopupRounding        = 6.0f;        /* 浮层 = 最大圆角 = 层级提示 */
        style.PopupBorderSize      = 1.0f;

        /* ---- 控件：统一的 4px 圆角 + 一档细边框 ---- */
        style.FramePadding         = ImVec2(8.0f, 4.0f);  /* 控件更"厚实"，约 22px 行高 */
        style.FrameRounding        = 4.0f;
        style.FrameBorderSize      = 1.0f;        /* 输入框有清晰边界 */
        style.ItemSpacing          = ImVec2(8.0f, 6.0f);  /* 行距呼吸感 */
        style.ItemInnerSpacing     = ImVec2(6.0f, 4.0f);
        style.CellPadding          = ImVec2(6.0f, 3.0f);
        style.TouchExtraPadding    = ImVec2(0.0f, 0.0f);

        /* ---- 列表 / 树 ---- */
        style.IndentSpacing        = 18.0f;       /* 树层级更紧凑 */
        style.ColumnsMinSpacing    = 8.0f;
        style.SelectableTextAlign  = ImVec2(0.0f, 0.5f); /* 行文本垂直居中（如不习惯改回 0,0） */

        /* ---- 滚动条：细 + 圆 ---- */
        style.ScrollbarSize        = 11.0f;
        style.ScrollbarRounding    = 6.0f;
        style.GrabMinSize          = 12.0f;
        style.GrabRounding         = 4.0f;

        /* ---- Tab ---- */
        style.TabRounding          = 4.0f;
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

    inline void ApplyDark()
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style = ImGuiStyle(); /* 先回到官方默认值，避免残留旧配色 */
        ApplyMetrics(style);

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

        c[ImGuiCol_CheckMark]              = Token::Accent;
        c[ImGuiCol_SliderGrab]             = Token::Neutral7;
        c[ImGuiCol_SliderGrabActive]       = Token::Accent;                 /* 拖拽滑块 = 强调 */

        c[ImGuiCol_Button]                 = WithAlpha(Token::Neutral5, 0.90f);
        c[ImGuiCol_ButtonHovered]          = Token::Neutral5;
        c[ImGuiCol_ButtonActive]           = Token::Neutral6;

        /* 列表行：选中 = 强调色浸染；悬停 = 中性提亮；按下 = 强调加强 */
        c[ImGuiCol_Header]                 = WithAlpha(Token::Accent, 0.32f);
        c[ImGuiCol_HeaderHovered]          = Token::White06;
        c[ImGuiCol_HeaderActive]           = WithAlpha(Token::Accent, 0.48f);

        c[ImGuiCol_Separator]              = Token::Separator;
        c[ImGuiCol_SeparatorHovered]       = WithAlpha(Token::Accent, 0.60f);
        c[ImGuiCol_SeparatorActive]        = Token::Accent;

        c[ImGuiCol_ResizeGrip]             = WithAlpha(Token::Accent, 0.12f);
        c[ImGuiCol_ResizeGripHovered]      = WithAlpha(Token::Accent, 0.45f);
        c[ImGuiCol_ResizeGripActive]       = WithAlpha(Token::Accent, 0.80f);

        /* Tab：亮度单调递增 —— 条(Neutral1) < 未激活(Neutral2) < 激活(Neutral3)。
         * 激活项永远最亮，避免"亮的条 + 暗的 Tab"反向对比产生的下凹感。 */
        c[ImGuiCol_Tab]                    = Token::Neutral2;               /* 未激活 = 面板色，浮在条上 */
        c[ImGuiCol_TabHovered]             = Token::Neutral4;
        c[ImGuiCol_TabActive]              = Token::Neutral3;               /* 激活 = 全条最亮（抬起） */
        c[ImGuiCol_TabUnfocused]           = Token::Neutral1;               /* 未聚焦节点：条为 N0，Tab 亮一档 */
        c[ImGuiCol_TabUnfocusedActive]     = WithAlpha(Token::Neutral3, 0.80f);

        c[ImGuiCol_DockingPreview]         = WithAlpha(Token::Accent, 0.55f);
        c[ImGuiCol_DockingEmptyBg]         = Token::Neutral0;               /* 停靠空区 = 最深底 */

        c[ImGuiCol_PlotLines]              = Token::Neutral7;
        c[ImGuiCol_PlotLinesHovered]       = Token::AccentHover;
        c[ImGuiCol_PlotHistogram]          = WithAlpha(Token::Accent, 0.90f);
        c[ImGuiCol_PlotHistogramHovered]   = Token::AccentHover;

        c[ImGuiCol_TableHeaderBg]          = Token::Neutral3;
        c[ImGuiCol_TableBorderStrong]      = ImVec4(0.227f, 0.251f, 0.282f, 1.00f);
        c[ImGuiCol_TableBorderLight]       = ImVec4(0.165f, 0.184f, 0.212f, 1.00f);
        c[ImGuiCol_TableRowBg]             = Token::Clear;
        c[ImGuiCol_TableRowBgAlt]          = ImVec4(1.0f, 1.0f, 1.0f, 0.03f);

        c[ImGuiCol_TextSelectedBg]         = WithAlpha(Token::Accent, 0.38f);
        c[ImGuiCol_DragDropTarget]         = WithAlpha(Token::Accent, 0.92f);
        c[ImGuiCol_NavHighlight]           = Token::Accent;
        c[ImGuiCol_NavWindowingHighlight]  = ImVec4(1.0f, 1.0f, 1.0f, 0.70f);
        c[ImGuiCol_NavWindowingDimBg]      = ImVec4(0.0f, 0.0f, 0.0f, 0.35f);
        c[ImGuiCol_ModalWindowDimBg]       = ImVec4(0.03f, 0.03f, 0.04f, 0.55f);
    }

    /* ==================== 字体系统 ==================== */

    /* 两个要点：中文要显式传 GlyphRanges（默认只含 Latin，中文会渲染成 '?'）；Retina 按 dpi_scale
     * 倍光栅化 + font->Scale 缩回逻辑尺寸（2x 屏原生渲染、不糊）。
     * 字体组合：OpenSans（拉丁）+ msyh（常用中文 2500 字），先到先得、msyh 用 MergeMode 补缺。 */
    struct EditorFonts
    {
        ImFont* Regular = nullptr; /* 正文（逻辑 14px） */
        ImFont* Title   = nullptr; /* 面板标题（逻辑 16px） */
    };

    /* 全局字体存取：各面板用 GetFonts().Title 切换标题字体 */
    namespace Detail
    {
        inline EditorFonts& FontsStorage()
        {
            static EditorFonts s_Fonts;
            return s_Fonts;
        }
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

        font->Scale = 1.0f / dpi_scale; /* 光栅化保留 dpi 倍细节，逻辑尺寸不变 */
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

        const std::string latin_regular = font_dir + "/opensans/OpenSans-Regular.ttf";
        const std::string latin_semibold = font_dir + "/opensans/OpenSans-SemiBold.ttf";
        const std::string cjk = font_dir + "/msyh.ttf";

        fonts.Regular = AddMergedFont(io, latin_regular, cjk, 14.0f, dpi_scale);
        fonts.Title   = AddMergedFont(io, latin_semibold, cjk, 16.0f, dpi_scale);

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
