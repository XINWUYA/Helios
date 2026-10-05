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

	/* 删除前记录父子关系：销毁会把子节点提升到根层级，销毁后再问就晚了 */
	void DeleteEntityCommand::CaptureHierarchy(Entity entity)
	{
		m_Parent = m_Scene->GetParent(entity);

		const auto parent_view = m_Scene->GetRegistry().view<ParentComponent>();
		for (const entt::entity candidate : parent_view)
		{
			if (parent_view.get<ParentComponent>(candidate).m_Parent != static_cast<entt::entity>(entity))
				continue;

			ChildLink link;
			link.Child = candidate;

			Entity child{ candidate, m_Scene };
			if (child.HasComponent<TransformComponent>())
				link.Local = child.GetComponent<TransformComponent>();

			m_Children.emplace_back(link);
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
			CaptureHierarchy(m_Entity);
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

		/* 层级一并还原：父节点可能已经不在了，这时退化为根节点 */
		if (m_Scene->IsEntityValid(m_Parent))
			m_Scene->SetParentLink(m_Entity, m_Parent);

		for (const ChildLink& link : m_Children)
		{
			if (!m_Scene->IsEntityValid(link.Child))
				continue;

			/* 先还原本地变换再挂回去：要的是"层级与位置都回到删除前"，
			 * 而不是按当前状态反推（子节点的世界变换在删除时已按根层级重算过）。 */
			Entity child{ link.Child, m_Scene };
			if (child.HasComponent<TransformComponent>())
				child.GetComponent<TransformComponent>() = link.Local;

			m_Scene->SetParentLink(child, m_Entity);
		}
	}
}
