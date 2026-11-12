#pragma once

#include <Helios.h>

namespace Helios
{
	/* 玩家形态的场景层：载入项目配置里的启动场景，每帧直接渲染到默认目标。
	 * 没有编辑器面板与 ImGui，HUD 之后交给运行时 UI。 */
	class PlayerLayer final : public ILayer
	{
	public:
		PlayerLayer();

		void OnAttached() override;
		void OnDetached() override;
		void OnUpdate(float delta_time) override;
		void OnEvent(IEvent* event) override;

	private:
		[[nodiscard]] Camera* GetCamera() const;
		/* 视口与相机比例按窗口物理像素走 */
		void ApplyViewport();
		/* 指针交给场景画布：HUD 的按钮与摇杆要在 Player 里也能按 */
		void ForwardUIInput();
		/* 触摸落点是否落在画布控件上，决定这根手指归 UI 还是归视角 */
		[[nodiscard]] bool HitTestUI(const glm::vec2& point) const;
		bool OnWindowResize(WindowResizeEvent* event);

		SharedPtr<Scene> m_pScene{ nullptr };
		entt::entity m_Camera{ entt::null };
		/* 三个按键上一帧的按下状态，用来判按下沿与抬起沿 */
		bool m_MouseDown[3]{ false, false, false };
	};
}
