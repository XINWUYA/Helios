#pragma once
#include <filesystem>
#include <string>
#include "Helios/Command/Command.h"
#include "Command/AssetFileOps.h"

namespace Helios
{
	/* 新建文件夹：撤销只删"还是空目录"的那一个 —— 用户可能已经在里面放了东西，宁可留个多余的
	 * 空目录、也不删掉文件。 */
	class CreateAssetFolderCommand final : public ICommand
	{
	public:
		/* folder 是绝对路径（= Assets / 相对路径）；sink 可为空（只改磁盘、不通知界面） */
		CreateAssetFolderCommand(IAssetChangeSink* sink, const std::filesystem::path& folder);

		void Do() override;
		void Undo() override;

		[[nodiscard]] const char* GetLabel() const override { return m_Label.c_str(); }
		/* 只动磁盘上的资源，不改场景内容（否则"场景未保存"会被它带出来） */
		[[nodiscard]] bool AffectsSceneDocument() const override { return false; }

		[[nodiscard]] const std::filesystem::path& GetFolder() const { return m_Folder; }

	private:
		void Notify(const std::string& from, const std::string& to);

		IAssetChangeSink* m_Sink{ nullptr };
		std::filesystem::path m_Folder;
		std::string m_Label;
	};
}
