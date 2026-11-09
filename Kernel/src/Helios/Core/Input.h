#pragma once
#include <glm/glm.hpp>
#include "KeyCodes.h"
#include "MouseCodes.h"

namespace Helios
{
	/* 游戏画面在屏幕上的区域（ImGui 单位）：鼠标按它换算成视口内坐标 */
	struct InputViewport
	{
		glm::vec2 Min{ 0.0f };   /* 左上角屏幕坐标 */
		glm::vec2 Size{ 0.0f };  /* 宽高；为 0 表示整个窗口 */
	};

	/* 输入：脚本侧读受「游戏焦点」这道闸控制，编辑器侧读原始状态。
	 * 闸关着（游戏画面没有焦点）时脚本读到的按键一律未按下、鼠标在原点，
	 * 于是在属性面板打字不会让角色跟着动；编辑器自己的快捷键与相机导航走 Raw 接口，
	 * 不受影响。 */
	class HELIOS_API Input
	{
	public:
		/* 由拥有游戏画面的宿主每帧写入（编辑器 / Player）。focused=false 即闸关闭；
		 * viewport 给出画面区域，鼠标据此换算成画面内坐标与 0 到 1 的归一化坐标。
		 * 不调用时默认「聚焦 + 整个窗口」，Samples 与无界面工具的行为不变。
		 * 另一个条件（应用窗口本身有没有系统焦点）由 Input 内部判断，不需要传。 */
		static void SetGameInput(bool focused, const InputViewport& viewport);

		/* ---- 脚本侧：走闸 ---- */
		static bool IsKeyPressed(const KeyCode key);
		static bool IsMouseButtonPressed(const MouseCode btn);
		/* 画面内坐标（左上角为原点）；闸关着时是原点 */
		static glm::vec2 GetMousePos();
		static float GetMouseX();
		static float GetMouseY();
		/* 画面内归一化坐标（0 到 1）；没有画面区域或闸关着时是原点 */
		static glm::vec2 GetMousePosNormalized();

		/* ---- 编辑器侧：不看闸 ---- */
		static bool IsKeyPressedRaw(const KeyCode key);
		static bool IsMouseButtonPressedRaw(const MouseCode btn);
		/* 窗口坐标（左上角为原点） */
		static glm::vec2 GetMousePosRaw();
	};
}
