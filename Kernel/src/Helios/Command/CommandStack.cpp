#include "Pch.h"
#include "CommandStack.h"

namespace Helios
{
	void CommandStack::Execute(UniquePtr<ICommand> command)
	{
		if (command == nullptr)
			return;

		command->Do();

		/* 新操作产生后，原有的重做分支失效 */
		m_Commands.resize(m_NextIndex);
		m_CommandTransactions.resize(m_NextIndex);

		/* 合并窗口内尝试并入栈顶：一次拖拽的每帧改动只留一条历史。
		 * 要求栈顶与本条属于同一事务，避免跨过上一次拖拽。 */
		const bool can_merge =
			m_InTransaction &&
			!m_Commands.empty() &&
			m_CommandTransactions.back() == m_TransactionId &&
			m_Commands.back()->TryMerge(*command);

		if (can_merge)
			return;

		m_Commands.emplace_back(std::move(command));
		m_CommandTransactions.emplace_back(m_TransactionId);
		m_NextIndex = m_Commands.size();
	}

	void CommandStack::BeginTransaction()
	{
		/* 每次进入合并窗口都换一个编号：上一次拖拽落下的历史不会被本次吸收 */
		++m_TransactionId;
		m_InTransaction = true;
	}

	void CommandStack::EndTransaction()
	{
		m_InTransaction = false;
	}

	bool CommandStack::Undo()
	{
		if (!CanUndo())
			return false;

		--m_NextIndex;
		m_Commands[m_NextIndex]->Undo();
		return true;
	}

	bool CommandStack::Redo()
	{
		if (!CanRedo())
			return false;

		m_Commands[m_NextIndex]->Do();
		++m_NextIndex;
		return true;
	}

	void CommandStack::Clear()
	{
		m_Commands.clear();
		m_CommandTransactions.clear();
		m_NextIndex = 0;
		m_InTransaction = false;
	}

	const char* CommandStack::GetUndoLabel() const
	{
		return CanUndo() ? m_Commands[m_NextIndex - 1]->GetLabel() : nullptr;
	}

	const char* CommandStack::GetRedoLabel() const
	{
		return CanRedo() ? m_Commands[m_NextIndex]->GetLabel() : nullptr;
	}
}
