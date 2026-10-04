#include "Pch.h"
#include "Command/CreateEntityCommand.h"

namespace Helios
{
	CreateEntityCommand::CreateEntityCommand(SharedPtr<Scene> scene, const char* name,
	                                         const std::vector<AddFunc>& components)
		: m_Scene(std::move(scene)), m_Components(components)
	{
		m_Name = (name != nullptr) ? name : std::string();
		m_Label = std::string("Create ") + (m_Name.empty() ? "Entity" : m_Name);
	}

	void CreateEntityCommand::Do()
	{
		if (m_Scene == nullptr)
			return;

		m_Entity = m_Scene->CreateEntity(m_Name);

		for (const AddFunc add : m_Components)
		{
			if (add != nullptr)
				add(m_Entity);
		}
	}

	void CreateEntityCommand::Undo()
	{
		if (m_Scene == nullptr || !m_Entity)
			return;

		m_Scene->DestroyEntity(m_Entity);
		m_Entity = {};
	}
}
