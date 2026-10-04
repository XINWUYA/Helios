#pragma once
#include <string>
#include <vector>
#include "Helios/Command/Command.h"
#include "Helios/Reflection/ComponentRegistry.h"
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

		/* 采集实体上所有已注册组件的数据快照 */
		void CaptureComponents(Entity entity);

		SharedPtr<Scene>               m_Scene;
		Entity                         m_Entity;
		std::string                    m_Name;
		std::vector<CapturedComponent> m_Components;
		bool                           m_Captured{ false };
		std::string                    m_Label;
	};
}
