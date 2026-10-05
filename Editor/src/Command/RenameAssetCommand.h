#pragma once
#include <filesystem>
#include <string>
#include "Helios/Command/Command.h"
#include "Command/AssetFileOps.h"

namespace Helios
{
	/* 重命名 / 移动资源：撤销即改回来。
	 * 名字合法性由界面在开命令之前挡住（见 AssetNameError），这里只做落盘。 */
	class RenameAssetCommand final : public ICommand
	{
	public:
		/* from / to 都是绝对路径；sink 可为空（只改磁盘、不通知界面） */
		RenameAssetCommand(IAssetChangeSink* sink, const std::filesystem::path& from,
			const std::filesystem::path& to);

		void Do() override;
		void Undo() override;

		[[nodiscard]] const char* GetLabel() const override { return m_Label.c_str(); }
		/* 只动磁盘上的资源，不改场景内容（否则"场景未保存"会被它带出来） */
		[[nodiscard]] bool AffectsSceneDocument() const override { return false; }

	private:
		/* 改名成 / 改回来；失败只记日志（文件可能已经被编辑器之外的操作动过了） */
		void Rename(const std::filesystem::path& from, const std::filesystem::path& to);

		IAssetChangeSink* m_Sink{ nullptr };
		std::filesystem::path m_From;
		std::filesystem::path m_To;
		std::string m_Label;
	};
}
