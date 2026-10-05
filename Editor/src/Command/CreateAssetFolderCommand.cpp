#include "Pch.h"
#include "Command/CreateAssetFolderCommand.h"

namespace Helios
{
	CreateAssetFolderCommand::CreateAssetFolderCommand(IAssetChangeSink* sink, const std::filesystem::path& folder)
		: m_Sink(sink), m_Folder(folder)
	{
		m_Label = std::string("Create ") + PathToUtf8(m_Folder.filename());
	}

	void CreateAssetFolderCommand::Do()
	{
		std::error_code error;
		std::filesystem::create_directories(m_Folder, error);

		if (error)
		{
			CORE_LOG_ERROR("新建文件夹失败：{0}（{1}）", PathToUtf8(m_Folder), error.message());
			return;
		}

		Notify(std::string(), AssetRelativePath(m_Folder));
	}

	void CreateAssetFolderCommand::Undo()
	{
		std::error_code error;
		if (!std::filesystem::is_directory(m_Folder, error) || error)
			return;

		/* 只删空目录：里面有东西就不是"撤销新建"，而是"删掉别人放进去的资产"了 */
		const std::filesystem::directory_iterator entry(m_Folder, error);
		if (error || entry != std::filesystem::directory_iterator())
		{
			CORE_LOG_WARN("撤销新建文件夹时目录已非空，保持原样：{0}", PathToUtf8(m_Folder));
			return;
		}

		std::filesystem::remove(m_Folder, error);
		if (error)
		{
			CORE_LOG_ERROR("撤销新建文件夹失败：{0}（{1}）", PathToUtf8(m_Folder), error.message());
			return;
		}

		Notify(AssetRelativePath(m_Folder), std::string());
	}

	void CreateAssetFolderCommand::Notify(const std::string& from, const std::string& to)
	{
		if (m_Sink != nullptr)
			m_Sink->OnAssetPathChanged(from, to);
	}
}
