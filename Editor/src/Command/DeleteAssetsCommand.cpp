#include "Pch.h"
#include "Command/DeleteAssetsCommand.h"

namespace Helios
{
	DeleteAssetsCommand::DeleteAssetsCommand(IAssetChangeSink* sink, const std::filesystem::path& trash_root,
		std::vector<std::filesystem::path> paths)
		: m_Sink(sink), m_TrashRoot(trash_root)
	{
		m_Entries.reserve(paths.size());
		for (std::filesystem::path& path : paths)
			m_Entries.push_back(Entry{ std::move(path), std::filesystem::path() });

		if (m_Entries.size() == 1)
			m_Label = std::string("Delete ") + PathToUtf8(m_Entries.front().Path.filename());
		else
			m_Label = "Delete " + std::to_string(m_Entries.size()) + " items";
	}

	void DeleteAssetsCommand::Do()
	{
		for (Entry& entry : m_Entries)
		{
			entry.Trashed.clear();

			std::string error;
			if (!MoveAssetToTrash(m_TrashRoot, entry.Path, entry.Trashed, error))
			{
				/* 单个失败不放倒整批：能删的照删，撤销时也只搬回真删掉的那些 */
				CORE_LOG_ERROR("删除失败：{0}", error);
				continue;
			}

			if (m_Sink != nullptr)
				m_Sink->OnAssetPathChanged(AssetRelativePath(entry.Path), std::string());
		}
	}

	void DeleteAssetsCommand::Undo()
	{
		/* 倒着搬回：批次里若同时有目录与它下面的东西（理论上不会，界面只列同一层），
		 * 先恢复外层再恢复内层才不会撞到"父目录还不存在"。 */
		for (auto entry = m_Entries.rbegin(); entry != m_Entries.rend(); ++entry)
		{
			if (entry->Trashed.empty())
				continue;   /* Do 时就没搬成功（或已经搬回来过） */

			std::string error;
			if (!RestoreAssetFromTrash(entry->Trashed, entry->Path, error))
			{
				CORE_LOG_ERROR("撤销删除失败：{0}", error);
				continue;
			}

			entry->Trashed.clear();   /* 重做时重新挑一个落点 */

			if (m_Sink != nullptr)
				m_Sink->OnAssetPathChanged(std::string(), AssetRelativePath(entry->Path));
		}
	}
}
