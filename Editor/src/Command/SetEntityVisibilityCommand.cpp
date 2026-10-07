#include "Pch.h"
#include "SetEntityVisibilityCommand.h"

namespace Helios
{
	SetEntityVisibilityCommand::SetEntityVisibilityCommand(SharedPtr<Scene> scene, Entity entity, bool visible)
		: m_Scene(std::move(scene)), m_Entity(entity), m_TargetVisible(visible)
	{
		std::string name = "(Entity)";
		if (m_Entity && m_Entity.HasComponent<NameComponent>())
			name = m_Entity.GetComponent<NameComponent>().m_Name;

		m_Label = std::string(visible ? "Show " : "Hide ") + name;
	}

	void SetEntityVisibilityCommand::Do()
	{
		if (m_Scene == nullptr || !m_Scene->IsEntityValid(m_Entity))
			return;

		/* 首次执行时记录原状（与 DeleteEntityCommand 同一套"重建后沿用同一份快照"的写法）：
		 * 记住组件是否在场，撤销才能把"缺席 = 可见"的原样原封不动还回去。 */
		if (!m_Captured)
		{
			m_BeforeExisted = m_Entity.HasComponent<VisibilityComponent>();
			m_BeforeVisible = !m_BeforeExisted || m_Entity.GetComponent<VisibilityComponent>().m_Visible;
			m_Captured = true;
		}

		Apply(m_TargetVisible, /*keep_component=*/false);
	}

	void SetEntityVisibilityCommand::Undo()
	{
		if (m_Scene == nullptr || !m_Scene->IsEntityValid(m_Entity))
			return;

		Apply(m_BeforeVisible, m_BeforeExisted);
	}

	void SetEntityVisibilityCommand::Apply(bool visible, bool keep_component)
	{
		/* 显示 = 去组件（缺席即可见）；隐藏 = 组件在场且 m_Visible=false */
		if (visible && !keep_component)
		{
			if (m_Entity.HasComponent<VisibilityComponent>())
				m_Entity.RemoveComponent<VisibilityComponent>();
			return;
		}

		VisibilityComponent& visibility = m_Entity.HasComponent<VisibilityComponent>()
			? m_Entity.GetComponent<VisibilityComponent>()
			: m_Entity.AddComponent<VisibilityComponent>();
		visibility.m_Visible = visible;
	}
}
