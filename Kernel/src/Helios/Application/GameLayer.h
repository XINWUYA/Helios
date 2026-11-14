#pragma once

#include <string>

#include "Helios/Core/Layer.h"
#include "Helios/Scene/Scene.h"
#include "Helios/Script/ProjectScripts.h"

namespace Helios
{
	class Camera;
	class KeyPressedEvent;
	class KeyReleasedEvent;
	class MouseScrolledEvent;
	class WindowResizeEvent;

	/* 运行当前项目的游戏层：载入启动场景、挂上项目脚本、把指针和按键转给场景画布。
	 * 启动场景可由命令行 --scene 覆盖。Player 直接用它；示例或新项目要叠自己的界面就继承一层，别再抄一份 */
	class HELIOS_API GameLayer : public ILayer
	{
	public:
		/* output_to_texture 打开时画面进视图纹理，由宿主自己拿去显示；关着直出窗口 */
		explicit GameLayer(bool output_to_texture = false);

		void OnAttached() override;
		void OnDetached() override;
		void OnUpdate(float delta_time) override;
		void OnEvent(IEvent* event) override;

	protected:
		/* 宿主界面占着输入时返回 true（比如 ImGui 拿着鼠标或键盘），游戏画布就不收 */
		[[nodiscard]] virtual bool IsPointerBlocked() const { return false; }
		[[nodiscard]] virtual bool IsKeyboardBlocked() const { return false; }

		[[nodiscard]] Camera* GetCamera() const;
		[[nodiscard]] const std::string& GetScenePath() const { return m_ScenePath; }
		[[nodiscard]] const ProjectScripts& GetScripts() const { return m_Scripts; }
		/* 最近一次脚本异常，空表示没出过错 */
		[[nodiscard]] const std::string& GetScriptFault() const { return m_ScriptFault; }

	private:
		/* 视口铺满窗口，相机比例跟着窗口走 */
		void ApplyViewport();
		void ForwardUIInput();
		/* 触摸落点在画布控件上就归 UI，否则留给脚本转视角 */
		[[nodiscard]] bool HitTestUI(const glm::vec2& point) const;

		bool OnWindowResize(WindowResizeEvent* event);
		bool OnKeyPressed(KeyPressedEvent* event);
		bool OnKeyReleased(KeyReleasedEvent* event);
		bool OnMouseScrolled(MouseScrolledEvent* event);

		bool             m_OutputToTexture{ false };
		SharedPtr<Scene> m_pScene{ nullptr };
		entt::entity     m_Camera{ entt::null };
		ProjectScripts   m_Scripts{};
		std::string      m_ScenePath{};
		std::string      m_ScriptFault{};
		/* 三个按键上一帧的按下状态，用来判按下沿与抬起沿 */
		bool             m_MouseDown[3]{ false, false, false };
	};
}
