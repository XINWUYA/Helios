#pragma once
#include "Helios/Command/Command.h"
#include "Helios/Scene/Components.h"
#include "Helios/Scene/Entity.h"

namespace Helios
{
	/* 修改 Transform 的命令：记录前后完整快照。Gizmo 拖拽每帧一条、合并后整段只留一条；实体
	 * 已销毁就静默跳过。 */
	class TransformCommand final : public ICommand
	{
	public:
		TransformCommand(Entity entity, const TransformComponent& before, const TransformComponent& after);

		void Do() override;
		void Undo() override;

		/* 同一实体的连续改动可合并：保留最初的起点，吸收最新的落点 */
		bool TryMerge(const ICommand& next) override;

		[[nodiscard]] const char* GetLabel() const override { return "Transform"; }

	private:
		Entity             m_Entity;
		TransformComponent m_Before;
		TransformComponent m_After;
	};
}
