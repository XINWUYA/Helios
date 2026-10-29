#include "Pch.h"
#include "EditorTheme.h"

namespace Helios::EditorTheme
{
    /* 缩放、主题、字体和 Token 只在这里定义一份：写在头文件里的话
     * Windows 上 Kernel.dll 和 Editor.exe 各有一份，Editor 改了 Kernel 看不到 */
    namespace Detail
    {
        float& UiScaleStorage()
        {
            static float s_UiScale = 1.0f;
            return s_UiScale;
        }

        int& ThemeIndexStorage()
        {
            static int s_ThemeIndex = 0;
            return s_ThemeIndex;
        }

        EditorFonts& FontsStorage()
        {
            static EditorFonts s_Fonts;
            return s_Fonts;
        }
    }

    namespace Token
    {
        float PropertyLabelWidth   = PropertyLabelWidthDesign;
        float PropertyRowPadY      = PropertyRowPadYDesign;
        float CardPad              = CardPadDesign;
        float SliderTrackThickness = SliderTrackThicknessDesign;
        float SliderDotRadius      = SliderDotRadiusDesign;

        /* 强调色一族：实际值由文件末尾的 AccentBootstrap → RefreshAccentTokens 落定 */
        ImVec4 Accent;
        ImVec4 AccentHover;
        ImVec4 AccentDown;
        ImVec4 AccentInk;
        ImVec4 AccentTint;
        ImVec4 AccentOnLight;
        ImVec4 AccentPale;
        ImVec4 AccentAlt;
        ImVec4 AccentAltDeep;
        ImVec4 AccentAltTint;
        ImVec4 AccentAltPale;
        ImVec4 SliderValue;
        ImVec4 SliderHover;
    }

    ImVec4 RowHoverSelected;

    namespace
    {
        /* 静态初始化时先把派生态算好：没人调 SetTheme 也是完整的默认主题 */
        struct AccentBootstrap
        {
            AccentBootstrap() { RefreshAccentTokens(); }
        } s_AccentBootstrap;
    }
}
