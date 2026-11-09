#include "Pch.h"
#include "PlaySession.h"
#include "Helios/Scene/Components.h"
#include "Helios/Scene/Scene.h"
#include "Helios/Scene/TerrainField.h"
#include "Helios/Script/ScriptRuntime.h"

namespace Helios
{
	void PlaySession::SetEditScene(const SharedPtr<Scene>& scene)
	{
		m_pEditScene = scene;
	}

	SharedPtr<Scene> PlaySession::GetActiveScene() const
	{
		return (m_State == PlayState::Edit) ? m_pEditScene : m_pRuntimeScene;
	}

	bool PlaySession::Start(entt::entity selection)
	{
		if (m_State != PlayState::Edit || m_pEditScene == nullptr)
			return false;

		/* 克隆走序列化与反序列化：渲染对象按组件重建，两份世界不共享实例 */
		SharedPtr<Scene> runtime_scene = m_pEditScene->Clone();
		if (runtime_scene == nullptr)
		{
			EDITOR_LOG_ERROR("Failed to clone the editing scene for play mode.");
			return false;
		}

		m_pRuntimeScene = std::move(runtime_scene);
		m_SelectionHandle = selection;
		m_StepRequested = false;

		TransferTerrainHeights(*m_pEditScene, *m_pRuntimeScene);
		m_pRuntimeScene->GetScriptRuntime().Start(*m_pRuntimeScene);

		m_State = PlayState::Playing;
		return true;
	}

	void PlaySession::Stop()
	{
		if (m_State == PlayState::Edit)
			return;

		/* 先停脚本再丢世界：OnDestroy 里还能安全访问实体 */
		if (m_pRuntimeScene != nullptr)
			m_pRuntimeScene->GetScriptRuntime().Stop();

		m_pRuntimeScene.reset();
		m_State = PlayState::Edit;
		m_SelectionHandle = entt::null;
		m_StepRequested = false;
	}

	void PlaySession::SetPaused(bool paused)
	{
		if (paused && m_State == PlayState::Playing)
			m_State = PlayState::Paused;
		else if (!paused && m_State == PlayState::Paused)
			m_State = PlayState::Playing;
	}

	bool PlaySession::ConsumeStepRequest()
	{
		if (m_State != PlayState::Paused || !m_StepRequested)
			return false;

		m_StepRequested = false;
		return true;
	}

	/* 高度存在 TerrainField 里，不进 .scn：克隆拿不到，按句柄单独搬一遍。
	 * 搬运会把"有未保存改动"标记置真，运行世界的导出入口另有关闭兜住落盘 */
	void PlaySession::TransferTerrainHeights(Scene& edit_scene, Scene& runtime_scene)
	{
		for (auto [handle, terrain] : edit_scene.GetRegistry().view<TerrainComponent>().each())
		{
			if (terrain.m_Field == nullptr || terrain.m_Field->IsEmpty())
				continue;

			/* 克隆保留了句柄：同一个句柄就是同一个实体 */
			if (!runtime_scene.IsEntityValid(handle))
				continue;

			TerrainComponent* target = runtime_scene.GetRegistry().try_get<TerrainComponent>(handle);
			if (target == nullptr)
				continue;
			if (target->m_Field == nullptr)
				target->m_Field = CreateSharedPtr<TerrainField>();

			if (!target->m_Field->RestoreHeightState(*target, terrain.m_Field->CaptureHeightState()))
				EDITOR_LOG_WARN("Failed to carry terrain heights into play mode.");
		}
	}
}
