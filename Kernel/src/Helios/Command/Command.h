#pragma once

namespace Helios
{
	/* 编辑命令：编辑操作的写入口。Do / Undo 必须成对可逆；TryMerge 把连续的同目标改动（比如
	 * Gizmo 拖拽每帧产生的位移）合并成一条历史记录。 */
	class ICommand
	{
	public:
		virtual ~ICommand() = default;

		virtual void Do() = 0;
		virtual void Undo() = 0;

		/* 尝试把后续命令并入自身。
		 * 调用时 next 已经执行过 Do()，实现方只需吸收其目标状态。
		 * 返回 true 表示已吸收，next 不再单独入栈。 */
		virtual bool TryMerge(const ICommand& next)
		{
			(void)next;
			return false;
		}

		/* 历史记录里显示的名称 */
		[[nodiscard]] virtual const char* GetLabel() const = 0;

		/* 这条命令有没有改动场景文档：场景的"未保存"标记按它为真来数（见 GetSceneEditCount）。
		 * 资源浏览器的新建 / 重命名 / 删除也进同一条历史，但改的是磁盘资源、不是场景内容 —— 不该让
		 * 标题栏和层级面板跳出"场景已修改"。 */
		[[nodiscard]] virtual bool AffectsSceneDocument() const { return true; }
	};
}
