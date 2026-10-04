#include "Pch.h"
#include "AddComponentCommand.h"

namespace Helios
{
	AddComponentCommand::AddComponentCommand(Entity entity, const char* component_name, AddFunc add, RemoveFunc remove)
		: m_Entity(entity), m_Add(add), m_Remove(remove)
	{
		m_Label = std::string("Add ") + ((component_name != nullptr) ? component_name : "Component");
	}

	void AddComponentCommand::Do()
	{
		if (!m_Entity || m_Add == nullptr)
			return;

		m_Add(m_Entity);
	}

	void AddComponentCommand::Undo()
	{
		if (!m_Entity || m_Remove == nullptr)
			return;

		m_Remove(m_Entity);
	}
}
