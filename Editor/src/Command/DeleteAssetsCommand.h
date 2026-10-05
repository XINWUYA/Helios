#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include "Helios/Command/Command.h"
#include "Command/AssetFileOps.h"

namespace Helios
{
	/* 删除资源：移到项目回收站（见 AssetFileOps.h），撤销就搬回原处。一整批只留一条历史（多选
	 * 删 5 个，撤销一次全回来）；重做时会重新挑回收站落点，所以 Undo 会把落点清掉。 */
	class DeleteAssetsCommand final : public ICommand
	{
	public:
		/* paths 是绝对路径（同一批必须是同一个目录下的兄弟项，界面只可能这么选） */
		DeleteAssetsCommand(IAssetChangeSink* sink, const std::filesystem::path& trash_root,
			std::vector<std::filesystem::path> paths);

		void Do() override;
		void Undo() override;

		[[nodiscard]] const char* GetLabel() const override { return m_Label.c_str(); }
		/* 只动磁盘上的资源，不改场景内容（否则"场景未保存"会被它带出来） */
		[[nodiscard]] bool AffectsSceneDocument() const override { return false; }

	private:
		struct Entry
		{
			std::filesystem::path Path;      /* 原位置 */
			std::filesystem::path Trashed;   /* 搬到了回收站的哪儿（Do 时记下，Undo 用它搬回来） */
		};

		IAssetChangeSink* m_Sink{ nullptr };
		std::filesystem::path m_TrashRoot;
		std::vector<Entry> m_Entries;
		std::string m_Label;
	};
}
