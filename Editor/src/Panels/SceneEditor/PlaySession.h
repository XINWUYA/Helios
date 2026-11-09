#pragma once
#include <entt/entt.hpp>
#include <string>
#include "Common/EditorCommon.h"
#include "Helios/Common/Common.h"

namespace Helios
{
	class Scene;

	/* 运行会话：进 Play 克隆一份运行世界来跑，退出整份丢弃 —— 编辑世界在运行期间不被触碰，
	 * 撤销历史、选中、未保存的雕刻都天然保住。
	 * 状态与两份世界的归属都收在这里，面板只查状态、不存副本。 */
	class PlaySession
	{
	public:
		/* 编辑世界换了（首次装配 / 新建 / 打开）：调用方保证此时不在运行态 */
		void SetEditScene(const SharedPtr<Scene>& scene);
		[[nodiscard]] const SharedPtr<Scene>& GetEditScene() const { return m_pEditScene; }

		/* 进 Play：克隆编辑世界，把地形高度按句柄搬过去并启动脚本。
		 * selection 是进 Play 前的选中句柄，退出时按它接回。失败返回 false，编辑世界不动 */
		bool Start(entt::entity selection);
		/* 退出：停脚本，丢弃运行世界与运行期历史 */
		void Stop();

		[[nodiscard]] PlayState GetState() const { return m_State; }
		[[nodiscard]] bool IsPlaying() const { return m_State != PlayState::Edit; }
		[[nodiscard]] bool IsPaused() const { return m_State == PlayState::Paused; }

		/* 当前世界：编辑态是编辑世界，运行态是运行世界 */
		[[nodiscard]] SharedPtr<Scene> GetActiveScene() const;

		/* 进 Play 前选中的实体句柄：两份世界句柄一致，退出时按它接回选中 */
		[[nodiscard]] entt::entity GetSelectionHandle() const { return m_SelectionHandle; }

		void SetPaused(bool paused);
		/* 暂停中请求单步一帧（固定步长，可复现） */
		void RequestStep() { m_StepRequested = true; }
		/* 取走本帧的单步请求：只有暂停时才算数 */
		[[nodiscard]] bool ConsumeStepRequest();
		/* 单步的固定步长（秒） */
		static constexpr float kStepSeconds = 1.0f / 60.0f;

		/* 时间缩放：只作用于运行世界，渲染不受影响。进出 Play 复位成 1 */
		void SetTimeScale(float scale);
		[[nodiscard]] float GetTimeScale() const { return m_TimeScale; }

		/* 运行世界踩到脚本异常就自动暂停，别让一次脚本错误带走编辑器。
		 * 有异常时返回 true 并填 out_message（给工具栏提示用） */
		bool PauseOnScriptFault(std::string& out_message);

	private:
		/* 地形高度不在场景文件里：按句柄从编辑世界搬到运行世界 */
		static void TransferTerrainHeights(Scene& edit_scene, Scene& runtime_scene);
		/* 烘焙好的探针结果按句柄共享：运行世界不必重烤同一份 IBL */
		static void ShareBakedProbes(Scene& edit_scene, Scene& runtime_scene);

		PlayState m_State{ PlayState::Edit };
		SharedPtr<Scene> m_pEditScene;
		SharedPtr<Scene> m_pRuntimeScene;
		entt::entity m_SelectionHandle{ entt::null };
		bool m_StepRequested{ false };
		float m_TimeScale{ 1.0f };
	};
}
