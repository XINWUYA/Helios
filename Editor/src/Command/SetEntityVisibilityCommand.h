#pragma once
#include <string>
#include "Helios/Command/Command.h"
#include "Helios/Scene/Components.h"
#include "Helios/Scene/Entity.h"
#include "Helios/Scene/Scene.h"

namespace Helios
{
	/* 设置实体可见性（层级行右端的眼睛）。「隐藏」= 补上组件置 false；「显示」= 移除组件（没有
	 * 组件 = 可见，不留冗余记录）。撤销按执行前的原状还原。只写实体自己那一行 —— 父链的隐藏
	 * 不受影响（沿父链传染在渲染侧判定）。 */
	class SetEntityVisibilityCommand final : public ICommand
	{
	public:
		SetEntityVisibilityCommand(SharedPtr<Scene> scene, Entity entity, bool visible);

		void Do() override;
		void Undo() override;

		[[nodiscard]] const char* GetLabel() const override { return m_Label.c_str(); }

	private:
		/* keep_component：撤销还原"组件在场"的原状时为真；执行动作（Do）恒为假 ——
		 * 显示 = 去组件、隐藏 = 补组件并置 false，两种目标状态各自只有一个规范形。 */
		void Apply(bool visible, bool keep_component);

		SharedPtr<Scene> m_Scene;
		Entity           m_Entity;
		bool             m_TargetVisible{ true };
		/* 执行前的原状（首次 Do 时记录；重做沿用，别把撤销后的状态当原状） */
		bool             m_Captured{ false };
		bool             m_BeforeExisted{ false };
		bool             m_BeforeVisible{ true };
		std::string      m_Label;
	};
}
