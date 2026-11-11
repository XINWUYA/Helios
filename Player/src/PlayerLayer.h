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
		void OnUpdate(float delta_time) override;
		void OnEvent(IEvent* event) override;

	private:
		[[nodiscard]] Camera* GetCamera() const;
		/* 视口与相机比例按窗口物理像素走 */
		void ApplyViewport();
		bool OnWindowResize(WindowResizeEvent* event);

		SharedPtr<Scene> m_pScene{ nullptr };
		entt::entity m_Camera{ entt::null };
	};
}
