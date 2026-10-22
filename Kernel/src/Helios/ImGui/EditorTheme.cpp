#include "Pch.h"
#include "EditorTheme.h"

namespace Helios::EditorTheme
{
    /* 缩放、字体和 Token 尺寸只在这里定义一份：写在头文件里的话
     * Windows 上 Kernel.dll 和 Editor.exe 各有一份，Editor 改了 Kernel 看不到 */
    namespace Detail
    {
        float& UiScaleStorage()
        {
            static float s_UiScale = 1.0f;
            return s_UiScale;
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
    }
}
