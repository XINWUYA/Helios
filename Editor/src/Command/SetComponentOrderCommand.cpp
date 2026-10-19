#include "Pch.h"
#include "SetComponentOrderCommand.h"
#include "Helios/Scene/Components.h"

namespace Helios
{
	SetComponentOrderCommand::SetComponentOrderCommand(SharedPtr<Scene> scene, Entity entity,
		std::vector<std::string> before, std::vector<std::string> after)
		: m_Scene(std::move(scene)), m_Entity(entity),
		  m_Before(std::move(before)), m_After(std::move(after))
	{
		std::string name = "(Entity)";
		if (m_Entity && m_Entity.HasComponent<NameComponent>())
			name = m_Entity.GetComponent<NameComponent>().m_Name;

		m_Label = "Reorder " + name + " Components";
	}

	void SetComponentOrderCommand::Do()
	{
		if (m_Scene == nullptr || !m_Scene->IsEntityValid(m_Entity))
			return;

		m_Scene->SetComponentOrder(m_Entity, m_After);
	}

	void SetComponentOrderCommand::Undo()
	{
		if (m_Scene == nullptr || !m_Scene->IsEntityValid(m_Entity))
			return;

		m_Scene->SetComponentOrder(m_Entity, m_Before);
	}
}
