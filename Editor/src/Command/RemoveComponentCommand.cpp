#include "Pch.h"
#include "Command/RemoveComponentCommand.h"

namespace Helios
{
	RemoveComponentCommand::RemoveComponentCommand(Entity entity, const ComponentDesc& desc)
		: m_Entity(entity), m_Capture(desc.Capture), m_Restore(desc.Restore), m_Remove(desc.Remove)
	{
		const char* name = (desc.Name != nullptr) ? desc.Name : "Component";
		m_Label = std::string("Remove ") + name;
	}

	void RemoveComponentCommand::Do()
	{
		if (!m_Entity || m_Remove == nullptr)
			return;

		/* 先记录移除前的组件内容，撤销时据此还原 */
		if (!m_Captured)
		{
			if (m_Capture != nullptr)
				m_Capture(m_Entity, m_Snapshot);
			m_Captured = true;
		}

		m_Remove(m_Entity);
	}

	void RemoveComponentCommand::Undo()
	{
		if (!m_Entity || m_Restore == nullptr || !m_Snapshot.IsValid())
			return;

		m_Restore(m_Entity, m_Snapshot);
	}
}
