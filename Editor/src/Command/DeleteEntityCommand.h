#pragma once
#include <string>
#include <vector>
#include "Helios/Command/Command.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include "Helios/Scene/Components.h"
#include "Helios/Scene/Entity.h"
#include "Helios/Scene/Scene.h"

namespace Helios
{
	/* 删除实体：撤销时按快照重建（名称 + 全部组件 + 父子关系）。重建会得到新句柄，针对旧句柄的
	 * 命令会安全跳过；撤销后用 GetEntity() 取新句柄。 */
	class DeleteEntityCommand final : public ICommand
	{
	public:
		DeleteEntityCommand(SharedPtr<Scene> scene, Entity entity);

		void Do() override;
		void Undo() override;

		[[nodiscard]] const char* GetLabel() const override { return m_Label.c_str(); }

		/* 撤销后为新重建的实体 */
		[[nodiscard]] const Entity& GetEntity() const { return m_Entity; }

	private:
		struct CapturedComponent
		{
			ComponentRestoreFunc Restore{ nullptr };
			ComponentSnapshot    Snapshot;
		};

		/* 直接子节点：销毁父节点时它们会被提升到根层级（见 Scene::DestroyEntity），
		 * 撤销重建后要按原样挂回去，所以连当时的局部变换一起记下来。 */
		struct ChildLink
		{
			entt::entity       Child{ entt::null };
			TransformComponent Local;
		};

		/* 采集实体上所有已注册组件的数据快照 */
		void CaptureComponents(Entity entity);
		/* 采集父子关系：自己的父节点 + 直接子节点 */
		void CaptureHierarchy(Entity entity);

		SharedPtr<Scene>               m_Scene;
		Entity                         m_Entity;
		std::string                    m_Name;
		/* 删除前的父节点，撤销重建后按它挂回去 */
		entt::entity                   m_Parent{ entt::null };
		std::vector<ChildLink>         m_Children;
		std::vector<CapturedComponent> m_Components;
		bool                           m_Captured{ false };
		std::string                    m_Label;
	};
}
