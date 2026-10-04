#pragma once
#include <string>
#include <vector>
#include "Helios/Command/Command.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include "Helios/Scene/Entity.h"
#include "Helios/Scene/Scene.h"

namespace Helios
{
	/* 新建实体：按预设的名字和组件序列创建，撤销就是销毁。入栈之后能用 GetEntity() 取到新实体，
	 * 供调用方同步选中项。 */
	class CreateEntityCommand final : public ICommand
	{
	public:
		CreateEntityCommand(SharedPtr<Scene> scene, const char* name, const std::vector<AddFunc>& components);

		void Do() override;
		void Undo() override;

		[[nodiscard]] const char* GetLabel() const override { return m_Label.c_str(); }

		/* 当前实体；撤销后为空 */
		[[nodiscard]] const Entity& GetEntity() const { return m_Entity; }

	private:
		SharedPtr<Scene>     m_Scene;
		std::string          m_Name;
		std::vector<AddFunc> m_Components;
		Entity               m_Entity;
		std::string          m_Label;
	};
}
