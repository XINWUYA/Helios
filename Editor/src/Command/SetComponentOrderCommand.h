#pragma once
#include <string>
#include <vector>
#include "Helios/Command/Command.h"
#include "Helios/Scene/Entity.h"
#include "Helios/Scene/Scene.h"

namespace Helios
{
	/* 属性面板里的组件卡顺序：拖拽和"上移 / 下移"都走这条命令，
	 * 撤销与场景脏标记跟别的编辑一致（顺序随场景文件保存）。 */
	class SetComponentOrderCommand final : public ICommand
	{
	public:
		SetComponentOrderCommand(SharedPtr<Scene> scene, Entity entity,
			std::vector<std::string> before, std::vector<std::string> after);

		void Do() override;
		void Undo() override;

		[[nodiscard]] const char* GetLabel() const override { return m_Label.c_str(); }

	private:
		SharedPtr<Scene> m_Scene;
		Entity m_Entity;
		std::vector<std::string> m_Before;
		std::vector<std::string> m_After;
		std::string m_Label;
	};
}
