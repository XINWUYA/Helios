#pragma once
#include <Helios.h>

#include "Helios/Application/GameLayer.h"

namespace Helios
{
	/* 直接预览项目场景：场景默认取项目里配的启动场景，命令行 --scene 可另指一个。
	 * 在通用游戏层上叠一块 ImGui 统计面板，画面进视图纹理铺成面板背景，面板拿着鼠标或键盘时游戏输入让路 */
	class SampleSceneLayer final : public GameLayer
	{
	public:
		SampleSceneLayer();

		void OnUpdate(float delta_time) override;
		void OnImGuiRender() override;

	protected:
		[[nodiscard]] bool IsPointerBlocked() const override;
		[[nodiscard]] bool IsKeyboardBlocked() const override;
	};
}
