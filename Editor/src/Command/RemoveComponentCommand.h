#pragma once
#include <string>
#include "Helios/Command/Command.h"
#include "Helios/Reflection/ComponentRegistry.h"
#include "Helios/Scene/Entity.h"

namespace Helios
{
	/* 移除组件：撤销按数据快照还原。组件描述只有 Add / Remove 回调、没法还原数据，所以在首次
	 * 执行时拷进 ComponentSnapshot，撤销时写回。 */
	class RemoveComponentCommand final : public ICommand
	{
	public:
		RemoveComponentCommand(Entity entity, const ComponentDesc& desc);

		void Do() override;
		void Undo() override;

		[[nodiscard]] const char* GetLabel() const override { return m_Label.c_str(); }

	private:
		Entity               m_Entity;
		ComponentCaptureFunc m_Capture{ nullptr };
		ComponentRestoreFunc m_Restore{ nullptr };
		RemoveFunc           m_Remove{ nullptr };
		ComponentSnapshot    m_Snapshot;
		/* 快照只采一次：第一次 Do 记录移除前的状态 */
		bool                 m_Captured{ false };
		std::string          m_Label;
	};
}
