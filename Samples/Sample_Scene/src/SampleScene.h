#pragma once
#include <Helios.h>
#include "ProjectScripts.h"

namespace Helios
{
	/* 直接预览项目场景：场景自己的相机出画面，场景里的脚本按运行模式跑。
	 * 与编辑器 Play 一致：脚本实例入场时创建，画布收真实的指针与按键输入。 */
	class SampleSceneLayer final : public ILayer
	{
	public:
		SampleSceneLayer();
		~SampleSceneLayer() override;

		void OnAttached() override;
		void OnUpdate(float delta_time) override;
		void OnEvent(IEvent* event) override;
		void OnImGuiRender() override;

	private:
		[[nodiscard]] Camera* GetCamera() const;
		/* 视口铺满窗口，相机比例跟着窗口走 */
		void ApplyViewport();
		/* 指针转发给场景画布 */
		void ForwardUIInput();

		bool OnWindowResize(WindowResizeEvent* event);
		bool OnKeyPressed(KeyPressedEvent* event);
		bool OnKeyReleased(KeyReleasedEvent* event);
		bool OnMouseScrolled(MouseScrolledEvent* event);

		SharedPtr<Scene> m_pScene{ nullptr };
		entt::entity     m_Camera{ entt::null };
		ProjectScripts   m_Scripts{};
		bool             m_MouseDown[3]{};
		std::string      m_Fault{};
	};
}
