#pragma once
#include <string>
#include "Helios/Command/Command.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include "Helios/Scene/Entity.h"

namespace Helios
{
	/* 添加组件：撤销就是移除。接受的是具体的添加动作、而不是注册项 —— 同一组件的不同形态
	 * （三种光源）各有各的构造函数，动作由「Add 菜单」提供。 */
	class AddComponentCommand final : public ICommand
	{
	public:
		AddComponentCommand(Entity entity, const char* component_name, AddFunc add, RemoveFunc remove);

		void Do() override;
		void Undo() override;

		[[nodiscard]] const char* GetLabel() const override { return m_Label.c_str(); }

	private:
		Entity      m_Entity;
		AddFunc     m_Add{ nullptr };
		RemoveFunc  m_Remove{ nullptr };
		std::string m_Label;
	};
}
