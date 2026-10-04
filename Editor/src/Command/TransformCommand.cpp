#include "Pch.h"
#include "TransformCommand.h"

namespace Helios
{
	TransformCommand::TransformCommand(Entity entity, const TransformComponent& before, const TransformComponent& after)
		: m_Entity(entity), m_Before(before), m_After(after)
	{
	}

	void TransformCommand::Do()
	{
		if (!m_Entity || !m_Entity.HasComponent<TransformComponent>())
			return;

		m_Entity.GetComponent<TransformComponent>() = m_After;
	}

	void TransformCommand::Undo()
	{
		if (!m_Entity || !m_Entity.HasComponent<TransformComponent>())
			return;

		m_Entity.GetComponent<TransformComponent>() = m_Before;
	}

	bool TransformCommand::TryMerge(const ICommand& next)
	{
		const auto* other = dynamic_cast<const TransformCommand*>(&next);
		if (other == nullptr || !(other->m_Entity == m_Entity))
			return false;

		/* 保留最初的起点，吸收最新的落点 */
		m_After = other->m_After;
		return true;
	}
}
