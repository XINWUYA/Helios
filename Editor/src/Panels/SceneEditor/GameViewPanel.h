#pragma once
#include <imgui.h>

#include "Common/EditorCommon.h"
#include "Helios/Renderer/RenderCommon.h"
#include "EditorUI/EditorIcons.h"

namespace Helios
{
	/* Game 页签的版式：工具行（运行状态 / 时间缩放 / 画面比例 / 全屏）、画面框与空态。
	 * 只管画和收集意图，数据来源与落地归 SceneEditorLayer。 */
	class GameViewPanel
	{
	public:
		struct State
		{
			bool Playing{ false };
			bool Paused{ false };
			bool Fullscreen{ false };           /* 在全屏窗口里画：右端按钮换成退出全屏 */
			bool FixedAspect{ false };          /* 画面是否按固定比例显示 */
			float AspectRatio{ 16.0f / 9.0f };  /* 固定比例时的宽高比 */
			float TimeScale{ 1.0f };            /* 时间缩放，面板改了由调用方写回 */
			const char* Notice{ nullptr };      /* 空态主文案，非空表示没有画面 */
			const char* NoticeHint{ nullptr };  /* 空态副文案 */
			Icons::Id NoticeIcon{ Icons::Id::Play };
		};

		struct Result
		{
			ViewportRegion Region{};    /* 画面区域（屏幕坐标）：鼠标换算与相机视口都按它 */
			bool Drawable{ false };
			bool Hovered{ false };
			bool Focused{ false };
			bool ToggleFullscreen{ false };
		};

		/* 画一整页内容（调用方已 Begin 面板窗口；窗口内边距须为 0） */
		Result Draw(State& state, ImTextureID image);

	private:
		void DrawToolbar(State& state, Result& result);
		/* 比例按钮与它的弹层：Free、几个常用档位与自定义宽高 */
		void DrawAspectControl(State& state, const ImVec2& pos, float width, float height);
		void DrawNotice(const State& state, const ImVec2& min, const ImVec2& size) const;

		/* 自定义比例的输入草稿：弹层打开时从当前比例反推，之后跟着输入走 */
		int m_CustomWidth{ 16 };
		int m_CustomHeight{ 9 };
	};
}
