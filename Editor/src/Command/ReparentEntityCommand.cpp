#include "Pch.h"
#include "Command/ReparentEntityCommand.h"

namespace Helios
{
	ReparentEntityCommand::ReparentEntityCommand(SharedPtr<Scene> scene, Entity entity,
		entt::entity before_parent, const TransformComponent& before,
		entt::entity after_parent, const TransformComponent& after)
		: m_Scene(std::move(scene)), m_Entity(entity)
		, m_BeforeParent(before_parent), m_AfterParent(after_parent)
		, m_Before(before), m_After(after)
	{
	}

	void ReparentEntityCommand::Do()
	{
		Apply(m_AfterParent, m_After);
	}

	void ReparentEntityCommand::Undo()
	{
		Apply(m_BeforeParent, m_Before);
	}

	void ReparentEntityCommand::Apply(entt::entity parent, const TransformComponent& transform)
	{
		if (m_Scene == nullptr || !m_Scene->IsEntityValid(m_Entity))
			return;

		/* 父节点可能已经被删除（撤销顺序把删除排在前面的情况）：
		 * 退化成根节点，不往场景里写失效句柄。 */
		const entt::entity target =
			(parent != entt::null && m_Scene->IsEntityValid(parent)) ? parent : entt::null;

		m_Scene->SetParentLink(m_Entity, target);

		/* 组件可能被移除过：这时候只还原层级，不凭空把组件加回来 */
		if (m_Entity.HasComponent<TransformComponent>())
			m_Entity.GetComponent<TransformComponent>() = transform;
	}
}
