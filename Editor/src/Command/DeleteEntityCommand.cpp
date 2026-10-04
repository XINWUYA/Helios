#include "Pch.h"
#include "Command/DeleteEntityCommand.h"
#include "Helios/Scene/Components.h"

namespace Helios
{
	DeleteEntityCommand::DeleteEntityCommand(SharedPtr<Scene> scene, Entity entity)
		: m_Scene(std::move(scene)), m_Entity(entity)
	{
		m_Name = "(Entity)";
		if (m_Entity && m_Entity.HasComponent<NameComponent>())
			m_Name = m_Entity.GetComponent<NameComponent>().m_Name;

		m_Label = std::string("Delete ") + m_Name;
	}

	void DeleteEntityCommand::CaptureComponents(Entity entity)
	{
		for (const ComponentDesc& desc : ComponentRegistry::Instance().All())
		{
			if (desc.Capture == nullptr || desc.Has == nullptr || !desc.Has(entity))
				continue;

			CapturedComponent captured;
			captured.Restore = desc.Restore;
			desc.Capture(entity, captured.Snapshot);

			if (captured.Snapshot.IsValid())
				m_Components.emplace_back(std::move(captured));
		}
	}

	void DeleteEntityCommand::Do()
	{
		if (m_Scene == nullptr || !m_Entity)
			return;

		/* 首次执行时记录内容；重建后的再次删除沿用同一份快照 */
		if (!m_Captured)
		{
			CaptureComponents(m_Entity);
			m_Captured = true;
		}

		m_Scene->DestroyEntity(m_Entity);
	}

	void DeleteEntityCommand::Undo()
	{
		if (m_Scene == nullptr)
			return;

		Entity recreated = m_Scene->CreateEntity(m_Name);

		for (const CapturedComponent& captured : m_Components)
		{
			if (captured.Restore != nullptr)
				captured.Restore(recreated, captured.Snapshot);
		}

		m_Entity = recreated;
	}
}
