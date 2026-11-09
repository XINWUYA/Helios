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
		ShareBakedProbes(*m_pEditScene, *m_pRuntimeScene);
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
		/* 时间缩放是运行期工具，不跨越会话 */
		m_TimeScale = 1.0f;
	}

	void PlaySession::SetPaused(bool paused)
	{
		if (paused && m_State == PlayState::Playing)
			m_State = PlayState::Paused;
		else if (!paused && m_State == PlayState::Paused)
			m_State = PlayState::Playing;
	}

	void PlaySession::SetTimeScale(float scale)
	{
		m_TimeScale = std::clamp(scale, 0.0f, 4.0f);
	}

	bool PlaySession::ConsumeStepRequest()
	{
		if (m_State != PlayState::Paused || !m_StepRequested)
			return false;

		m_StepRequested = false;
		return true;
	}

	bool PlaySession::PauseOnScriptFault(std::string& out_message)
	{
		if (!IsPlaying() || m_pRuntimeScene == nullptr)
			return false;

		ScriptRuntime::Fault fault;
		if (!m_pRuntimeScene->GetScriptRuntime().ConsumeFault(fault))
			return false;

		/* 出错的实例已经被停掉了，这里只负责把整场暂停 + 说清楚是谁出的错 */
		SetPaused(true);

		out_message = "Script error in '" + fault.ClassName + "': " + fault.Message + ". Play is paused.";
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

	/* 烘焙好的探针按句柄共享结果：运行世界是克隆出来的，不共享就得把整份 IBL 重烤一遍，
	 * 场景里几个探针就是进 Play 时几秒的卡顿。接不上的（实时探针、配置不同）自己照常烘焙 */
	void PlaySession::ShareBakedProbes(Scene& edit_scene, Scene& runtime_scene)
	{
		for (auto [handle, probe] : edit_scene.GetRegistry().view<ReflectionProbeComponent>().each())
		{
			const SharedPtr<ReflectionProbe>& source = probe.m_ReflectionProbe;
			if (source == nullptr || !source->IsBaked())
				continue;

			if (!runtime_scene.IsEntityValid(handle))
				continue;

			ReflectionProbeComponent* target = runtime_scene.GetRegistry().try_get<ReflectionProbeComponent>(handle);
			if (target == nullptr || target->m_ReflectionProbe == nullptr)
				continue;

			target->m_ReflectionProbe->AdoptBakeResultFrom(*source);
		}
	}
}
