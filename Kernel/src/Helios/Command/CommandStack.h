#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include "Helios/Command/Command.h"
#include "Helios/Common/Common.h"

namespace Helios
{
	/* 命令栈：编辑历史的唯一管理者。位置模型：[0, m_NextIndex) 是已执行、[m_NextIndex, size) 是
	 * 可重做分支；Execute 会丢弃重做分支；合并窗口内的连续命令能并入栈顶。 */
	class CommandStack
	{
	public:
		CommandStack() = default;
		~CommandStack() = default;

		CommandStack(const CommandStack&) = delete;
		CommandStack& operator=(const CommandStack&) = delete;

		/* 执行并入栈；命令为空时不产生任何改动 */
		void Execute(UniquePtr<ICommand> command);

		/* 合并窗口：连续操作（如一次拖拽）开始 / 结束时调用。
		 * 窗口内新命令若能并入栈顶，则整段操作只留一条历史。 */
		void BeginTransaction();
		void EndTransaction();

		[[nodiscard]] bool CanUndo() const { return m_NextIndex > 0; }
		[[nodiscard]] bool CanRedo() const { return m_NextIndex < m_Commands.size(); }

		bool Undo();
		bool Redo();

		void Clear();

		[[nodiscard]] size_t GetHistorySize() const { return m_Commands.size(); }
		[[nodiscard]] size_t GetNextIndex() const { return m_NextIndex; }

		/* 供菜单显示 "Undo <标签>"；不可撤销时返回 nullptr */
		[[nodiscard]] const char* GetUndoLabel() const;
		[[nodiscard]] const char* GetRedoLabel() const;

	private:
		std::vector<UniquePtr<ICommand>> m_Commands;
		/* 每条命令所属的事务编号，与 m_Commands 一一对应。
		 * 只有编号与当前事务相同的相邻命令才允许合并 —— 否则上一段操作（如上一次拖拽）
		 * 会被本次吸收，撤销一次就退过头。 */
		std::vector<uint64_t> m_CommandTransactions;
		/* 下一条可重做的位置 */
		size_t m_NextIndex{ 0 };
		uint64_t m_TransactionId{ 0 };
		bool m_InTransaction{ false };
	};
}
