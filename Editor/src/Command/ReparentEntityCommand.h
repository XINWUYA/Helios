#pragma once
#include "Helios/Command/Command.h"
#include "Helios/Scene/Components.h"
#include "Helios/Scene/Entity.h"
#include "Helios/Scene/Scene.h"

namespace Helios
{
	/* 改变父子关系：记录父节点和局部变换的前后值。挂到新父节点要按新父空间重算局部变换（世界
	 * 变换不变），两者必须一起撤销；实体已销毁就静默跳过；父节点不存在则退化成根。 */
	class ReparentEntityCommand final : public ICommand
	{
	public:
		ReparentEntityCommand(SharedPtr<Scene> scene, Entity entity,
			entt::entity before_parent, const TransformComponent& before,
			entt::entity after_parent, const TransformComponent& after);

		void Do() override;
		void Undo() override;

		[[nodiscard]] const char* GetLabel() const override { return "Reparent"; }

	private:
		/* 落地一组「父节点 + 局部变换」 */
		void Apply(entt::entity parent, const TransformComponent& transform);

		SharedPtr<Scene>   m_Scene;
		Entity             m_Entity;
		entt::entity       m_BeforeParent{ entt::null };
		entt::entity       m_AfterParent{ entt::null };
		TransformComponent m_Before;
		TransformComponent m_After;
	};
}
